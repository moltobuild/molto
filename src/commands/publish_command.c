#include <molto/commands/publish_command.h>

#include <molto/exit_code.h>
#include <molto/services/credentials_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/recipe_service.h>
#include <molto/services/registry_service.h>
#include <molto/services/source_service.h>
#include <molto/util/doc.h>
#include <molto/util/toml.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_RECIPE "recipe.toml"
#define ARCHIVE_SUFFIX ".tar.zst"

#define COORDINATE_MAX 128
#define PATH_MAX_LEN 1024

/* The coordinate a recipe describes: what the registry will make immutable. */
typedef struct {
    char kind[COORDINATE_MAX];
    char name[COORDINATE_MAX];
    char version[COORDINATE_MAX];
    char target[COORDINATE_MAX];
    /* form = "source": the recipe is the whole artifact and there are no bytes
       to upload. Absent means "binary", which is what every recipe published
       before the key existed was. */
    bool from_source;
} coordinate;

static void report(const char *message) { fprintf(stderr, "molto: %s\n", message); }

/* --- reading the recipe --- */

static bool read_key(const toml_document *doc, const char *key, char *out, size_t size) {
    if(toml_get_string(doc, "", key, out, size))
        return true;
    fprintf(stderr, "molto: the recipe has no '%s'\n", key);
    return false;
}

/* The table each kind must carry. Checked here so a recipe missing it fails
   before a 45 MB upload rather than after it. */
static const char *required_table_of(const char *kind) {
    if(strcmp(kind, "toolchain") == 0)
        return "toolchain";
    if(strcmp(kind, "tool") == 0)
        return "tool";
    if(strcmp(kind, "package") == 0)
        return "package";
    return NULL;
}

/* The tables a source recipe carries instead: where it comes from, how it is
   built, and what a consumer gets. */
static const char *const SOURCE_TABLES[] = {"source", "build", "artifacts"};

static bool has_table(const toml_document *doc, const char *kind, const char *table) {
    if(toml_has_section(doc, table))
        return true;
    fprintf(stderr, "molto: a %s recipe needs a [%s] table\n", kind, table);
    return false;
}

/* Which mode the recipe is in. Declared, never inferred from which tables
   happen to be present: a source recipe with a misspelled [souce] would
   otherwise be a valid binary one whose archive merely went missing. */
static bool read_form(const toml_document *doc, bool *from_source) {
    char form[COORDINATE_MAX] = "";
    if(!toml_get_string(doc, "", "form", form, sizeof form)) {
        *from_source = false;
        return true;
    }
    if(strcmp(form, "binary") == 0) {
        *from_source = false;
        return true;
    }
    if(strcmp(form, "source") == 0) {
        *from_source = true;
        return true;
    }
    fprintf(stderr, "molto: unknown recipe form '%s'\n", form);
    return false;
}

/* A source recipe describes something to be built on the machine that wants
   it, which a toolchain and a tool exist precisely to avoid. */
static bool check_tables(const toml_document *doc, const coordinate *at) {
    if(at->from_source) {
        if(strcmp(at->kind, "package") != 0) {
            fprintf(stderr, "molto: a %s recipe must be form = \"binary\"\n", at->kind);
            return false;
        }
        for(size_t i = 0; i < sizeof SOURCE_TABLES / sizeof SOURCE_TABLES[0]; i++) {
            if(!has_table(doc, at->kind, SOURCE_TABLES[i]))
                return false;
        }
        return true;
    }

    const char *table = required_table_of(at->kind);
    if(table == NULL) {
        fprintf(stderr, "molto: unknown recipe kind '%s'\n", at->kind);
        return false;
    }
    return has_table(doc, at->kind, table);
}

/* What the tables say, read by exactly the code that will read them back.
 *
 * `check_tables` above asks whether a table is there; this asks whether it says
 * something a consumer can act on. They are different questions and only the
 * second one catches `system = "scons"`, a `type` nothing can build, a `std` no
 * compiler knows, or an `archive` with no digest beside it — each of which
 * publishes cleanly and then fails in the build of everyone who depends on it.
 *
 * Run here rather than left to the registry, which does validate and does agree
 * with these lists. The registry is the gate; this is the one that answers
 * before a request is made, which is what `--dry-run` offers to do and what
 * makes it worth running at all.
 *
 * The readers are the consumer's own, deliberately: a publisher and a consumer
 * disagreeing about what a recipe means is the failure this exists to prevent,
 * and two implementations of the same check is how that starts.
 *
 * `[artifacts]` and `[build]` are read for either form — an absent table is not
 * an error in either, so a binary recipe is checked for a malformed one rather
 * than excused. `[source]` is read only where there is one. */
static bool check_content(const toml_document *doc, const coordinate *at) {
    const doc_view view = doc_from_toml(doc);
    char err[256] = "";

    recipe_artifacts artifacts;
    if(!recipe_read_artifacts(view, &artifacts, err, sizeof err)) {
        report(err);
        return false;
    }

    recipe_build build;
    if(!recipe_read_build(view, &build, err, sizeof err)) {
        report(err);
        return false;
    }

    if(!at->from_source)
        return true;

    source_spec spec;
    if(!source_read(view, &spec, err, sizeof err)) {
        report(err);
        return false;
    }
    return true;
}

static bool read_coordinate(const char *path, coordinate *out) {
    char *text = fs_read_file(path);
    if(text == NULL) {
        fprintf(stderr, "molto: cannot read %s\n", path);
        return false;
    }

    char err[256] = "";
    toml_document *doc = toml_parse(text, err, sizeof err);
    free(text);
    if(doc == NULL) {
        fprintf(stderr, "molto: %s is not valid TOML: %s\n", path, err);
        return false;
    }

    bool ok = read_key(doc, "kind", out->kind, sizeof out->kind) &&
              read_key(doc, "name", out->name, sizeof out->name) &&
              read_key(doc, "version", out->version, sizeof out->version) &&
              read_key(doc, "target", out->target, sizeof out->target) &&
              read_form(doc, &out->from_source) && check_tables(doc, out) &&
              check_content(doc, out);

    toml_free(doc);
    return ok;
}

/* --- finding the archive --- */

/* The directory `path` lives in, or "." when it names a bare file. */
static void directory_of(const char *path, char *out, size_t size) {
    const char *slash = strrchr(path, '/');
    if(slash == NULL) {
        snprintf(out, size, ".");
        return;
    }
    const size_t length = (size_t)(slash - path);
    snprintf(out, size, "%.*s", (int)(length == 0 ? 1 : length), length == 0 ? "/" : path);
}

static bool ends_with(const char *text, const char *suffix) {
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length && strcmp(text + text_length - suffix_length, suffix) == 0;
}

/* The one archive beside the recipe. Two is an error rather than a guess:
   publishing the wrong bytes under a coordinate cannot be undone. */
static bool find_archive(const char *recipe_path, char *out, size_t size) {
    char dir[PATH_MAX_LEN];
    directory_of(recipe_path, dir, sizeof dir);

    DIR *handle = opendir(dir);
    if(handle == NULL) {
        fprintf(stderr, "molto: cannot read the directory %s\n", dir);
        return false;
    }

    char found[PATH_MAX_LEN] = "";
    size_t count = 0;
    for(const struct dirent *entry = readdir(handle); entry != NULL; entry = readdir(handle)) {
        if(!ends_with(entry->d_name, ARCHIVE_SUFFIX))
            continue;
        count++;
        if(count == 1 && !fs_format_path(found, sizeof found, "%s/%s", dir, entry->d_name)) {
            (void)closedir(handle);
            return fs_report_long_path("the archive path");
        }
    }
    (void)closedir(handle);

    if(count == 0) {
        fprintf(stderr, "molto: no %s archive in %s; name one with --file\n", ARCHIVE_SUFFIX, dir);
        return false;
    }
    if(count > 1) {
        fprintf(stderr, "molto: %s holds %zu archives; name the one to publish with --file\n", dir,
                count);
        return false;
    }
    snprintf(out, size, "%s", found);
    return true;
}

/* --- hashing --- */

/* Shelled out to for the same reason curl is: molto has no crypto, and a
   sha256 written here would be one more thing to get right and keep right. */
static bool checksum_of(const char *file, char *out, size_t size) {
    const char *argv[] = {"sha256sum", "--binary", file, NULL};
    char captured[256] = "";
    const int code = process_capture(argv, captured, sizeof captured);

    if(code == 127) {
        report("sha256sum is not installed, and molto needs it to publish");
        return false;
    }
    if(code != 0) {
        fprintf(stderr, "molto: could not hash %s\n", file);
        return false;
    }

    /* "<64 hex>  <name>" — only the digest is wanted. */
    const size_t digest_length = strcspn(captured, " \t\r\n");
    if(digest_length != 64 || digest_length >= size) {
        report("sha256sum did not answer with a digest");
        return false;
    }
    snprintf(out, size, "%.*s", (int)digest_length, captured);
    return true;
}

/* --- publishing --- */

static void describe(const coordinate *at, const char *registry) {
    fprintf(stderr, "Publishing %s %s@%s (%s)\n", at->kind, at->name, at->version, at->target);
    fprintf(stderr, "  registry %s\n", registry);
}

static void describe_archive(const char *archive, const char *checksum) {
    fprintf(stderr, "  archive  %s\n", archive);
    fprintf(stderr, "  sha256   %s\n", checksum);
}

static bool refused(const char *what, const registry_response *response) {
    char detail[512];
    registry_explain(response, detail, sizeof detail);
    fprintf(stderr, "molto: the registry refused the %s (%ld): %s\n", what, response->status,
            detail);
    return false;
}

static bool upload(const credentials *creds, const coordinate *at, const char *archive,
                   const char *checksum) {
    char path[PATH_MAX_LEN];
    if(!fs_format_path(path, sizeof path, "/v1/%ss/%s/%s/%s/blob", at->kind, at->name, at->version,
                       at->target))
        return fs_report_long_path("the upload path");

    registry_response response;
    char err[512] = "";
    if(!registry_upload_blob(creds->registry, creds->token, path, archive, checksum, &response, err,
                             sizeof err)) {
        report(err);
        return false;
    }
    if(response.status != 201)
        return refused("upload", &response);

    fprintf(stderr, "  uploaded\n");
    return true;
}

static bool record(const credentials *creds, const coordinate *at, const char *recipe) {
    char path[PATH_MAX_LEN];
    if(!fs_format_path(path, sizeof path, "/v1/%ss", at->kind))
        return fs_report_long_path("the publish path");

    registry_response response;
    char err[512] = "";
    if(!registry_publish_recipe(creds->registry, creds->token, path, recipe, &response, err,
                                sizeof err)) {
        report(err);
        return false;
    }
    if(response.status != 201)
        return refused("recipe", &response);

    return true;
}

/* A source recipe: one request and no bytes anywhere. The recipe is the whole
   artifact, so there is nothing to find, nothing to hash and nothing to
   upload -- which is also why naming an archive for one is a mistake worth
   reporting rather than an argument to ignore. */
static int publish_source(const coordinate *at, const char *recipe_path, const char *file,
                          bool dry_run) {
    if(file != NULL && file[0] != '\0') {
        report("a source recipe has no archive to publish; drop --file");
        return exit_usage_error;
    }

    credentials creds = {0};
    char err[256] = "";
    if(!credentials_load(&creds, err, sizeof err)) {
        report(err);
        return exit_dependency_failure;
    }

    describe(at, creds.registry);
    fprintf(stderr, "  source   recipe only, no archive\n");
    if(dry_run) {
        fprintf(stderr, "  dry run: nothing was sent\n");
        return exit_ok;
    }

    if(!record(&creds, at, recipe_path))
        return exit_dependency_failure;
    return exit_ok;
}

static int publish_binary(const coordinate *at, const char *recipe_path, const char *file,
                          bool dry_run) {
    char archive[PATH_MAX_LEN];
    if(file != NULL && file[0] != '\0')
        snprintf(archive, sizeof archive, "%s", file);
    else if(!find_archive(recipe_path, archive, sizeof archive))
        return exit_usage_error;

    if(!fs_path_exists(archive)) {
        fprintf(stderr, "molto: no such archive: %s\n", archive);
        return exit_usage_error;
    }

    char checksum[80];
    if(!checksum_of(archive, checksum, sizeof checksum))
        return exit_build_failure;

    credentials creds = {0};
    char err[256] = "";
    if(!credentials_load(&creds, err, sizeof err)) {
        report(err);
        return exit_dependency_failure;
    }

    describe(at, creds.registry);
    describe_archive(archive, checksum);
    if(dry_run) {
        fprintf(stderr, "  dry run: nothing was sent\n");
        return exit_ok;
    }

    /* The bytes first and the row last: a blob without a row is invisible and
       costs only storage, while a row without its blob is an artifact nobody
       can download and the registry cannot serve around. */
    if(!upload(&creds, at, archive, checksum))
        return exit_dependency_failure;
    if(!record(&creds, at, recipe_path))
        return exit_dependency_failure;
    return exit_ok;
}

int publish_command_run(const char *recipe, const char *file, bool dry_run) {
    const char *recipe_path = recipe != NULL && recipe[0] != '\0' ? recipe : DEFAULT_RECIPE;

    coordinate at = {0};
    if(!read_coordinate(recipe_path, &at))
        return exit_invalid_manifest;

    const int code = at.from_source ? publish_source(&at, recipe_path, file, dry_run)
                                    : publish_binary(&at, recipe_path, file, dry_run);
    if(code != exit_ok || dry_run)
        return code;

    fprintf(stderr, "Published %s %s@%s (%s)\n", at.kind, at.name, at.version, at.target);
    return exit_ok;
}
