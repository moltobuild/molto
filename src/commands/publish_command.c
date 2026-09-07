#include <molto/commands/publish_command.h>

#include <molto/exit_code.h>
#include <molto/services/credentials_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/pack_service.h>
#include <molto/services/recipe_service.h>
#include <molto/services/registry_service.h>
#include <molto/services/source_service.h>
#include <molto/util/doc.h>
#include <molto/util/progress.h>
#include <molto/util/semver.h>
#include <molto/util/sha256.h>
#include <molto/util/toml.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_RECIPE "recipe.toml"

/*
 * What an archive beside a recipe is called.
 *
 * `.tar.zst` was the only one, and was right about every artifact published
 * before a toolchain had to run on Windows. That one cannot be zstd: the
 * `tar.exe` Windows ships is bsdtar linked against zlib alone, so gzip is the
 * only packing it opens. Leaving the list at one meant `molto publish` in a
 * directory holding the gzip artifact reported that there was no archive in
 * it.
 *
 * Two entries and not a pattern, because "any file that looks like a tarball"
 * is how the wrong bytes get published under a coordinate that cannot be
 * republished.
 */
static const char *const ARCHIVE_SUFFIXES[] = {".tar.zst", ".tar.gz"};

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
    /* How the blob is packed, as the recipe declares it. Empty when it says
       nothing, which is every recipe written before a toolchain had to run on
       Windows and is read as the default for the target. */
    char format[COORDINATE_MAX];
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

/* A package's version has to be one a manifest can name.
 *
 * RFC-0009 lets a toolchain or a tool call itself what it likes — `13.2.0-x86_64`
 * is a real toolchain — but requires a `package` to be semver, because RFC-0008
 * orders versions to propose the newest. Checked here because a coordinate is
 * immutable: `pcre2@10.47` publishes cleanly against every other rule and then
 * cannot be depended on, since `[deps]` refuses two components as "not a
 * version". The registry does not catch it either, its pattern being about
 * characters rather than shape. */
static bool check_version(const coordinate *at) {
    if(strcmp(at->kind, "package") != 0)
        return true;

    semver parsed;
    if(semver_parse(at->version, &parsed))
        return true;

    fprintf(stderr,
            "molto: a package's version must be major.minor.patch and '%s' is not; nothing could "
            "depend on it, and a published coordinate cannot be taken back\n",
            at->version);
    return false;
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

    recipe_provide provide;
    if(!recipe_read_provide(view, &provide, err, sizeof err)) {
        report(err);
        return false;
    }
    /* Every other build system does its own configuration, so a recipe that
       both names one and supplies its output is saying two contradictory things
       about who is in charge (RFC-0009). */
    if(provide.count > 0 && build.system != recipe_build_none) {
        fprintf(stderr,
                "molto: the recipe provides %zu file%s and builds with %s, which does its own "
                "configuring; [[provide]] belongs to system = \"none\"\n",
                provide.count, provide.count == 1 ? "" : "s",
                recipe_build_system_name(build.system));
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
              read_form(doc, &out->from_source) && check_version(out) && check_tables(doc, out) &&
              check_content(doc, out);

    /* Optional, and read rather than inferred from a filename: the registry
       stores what the recipe declares, so this is the same statement molto
       will be held to. Absent means the default for the target. */
    if(!toml_get_string(doc, "", "format", out->format, sizeof out->format))
        out->format[0] = '\0';

    toml_free(doc);
    return ok;
}

/* --- finding the archive --- */

/* The directory `path` lives in, or "." when it names a bare file. */
static void directory_of(const char *path, char *out, size_t size) {
    const char *slash = strrchr(path, '/');
    /* Windows spells it the other way, and a native molto is handed native
       paths: `--recipe C:\publish\recipe.toml` has no forward slash in it at
       all, so looking for only one answered "." and sent the search for the
       archive to the current directory instead of to the one the recipe is
       in. Whichever separator comes last is the one that ends the directory,
       because Win32 accepts both and a path may carry a mixture. */
    const char *backslash = strrchr(path, '\\');
    if(backslash != NULL && (slash == NULL || backslash > slash))
        slash = backslash;
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

static bool looks_like_an_archive(const char *name) {
    for(size_t i = 0; i < sizeof ARCHIVE_SUFFIXES / sizeof ARCHIVE_SUFFIXES[0]; i++) {
        if(ends_with(name, ARCHIVE_SUFFIXES[i]))
            return true;
    }
    return false;
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
        if(!looks_like_an_archive(entry->d_name))
            continue;
        count++;
        if(count == 1 && !fs_format_path(found, sizeof found, "%s/%s", dir, entry->d_name)) {
            (void)closedir(handle);
            return fs_report_long_path("the archive path");
        }
    }
    (void)closedir(handle);

    if(count == 0) {
        fprintf(stderr, "molto: no %s or %s archive in %s; name one with --file\n",
                ARCHIVE_SUFFIXES[0], ARCHIVE_SUFFIXES[1], dir);
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

/* --- saying that something is happening --- */

/*
 * Publishing spends minutes in two places and used to print nothing in either.
 *
 * Hashing 127 MB and sending it are each long enough that a person watching a
 * still cursor cannot tell work from a hang -- which is exactly the report
 * that came back: the command "just sat there". Both have an honest total,
 * unlike the searches `util/progress.h` was written for, so both get a bar
 * rather than a spinner: the size of the file is known before the first byte
 * is read, and curl says how much of it has gone.
 *
 * Drawn only on a terminal. A bar in a log file is noise and a bar in a pipe
 * is corruption of whatever was being piped, which is why the choice is asked
 * once per step rather than assumed.
 */

/* Columns of bar. Narrow enough to leave the label and the figure room on an
   eighty-column terminal, which is the narrowest anyone still uses. */
#define STEP_BAR_CELLS 24

/* What a step has already shown, so it redraws only when the figure moves. */
typedef struct {
    int percent;
} publish_progress;

static void step_progress(const char *label, int percent) {
    char bar[PROGRESS_BAR_SIZE(STEP_BAR_CELLS)];
    (void)progress_bar_render(bar, sizeof bar, (size_t)percent, 100, STEP_BAR_CELLS);
    progress_erase_line(stderr);
    fprintf(stderr, "  %-8s %s %3d%%", label, bar, percent);
    (void)fflush(stderr);
}

/* Take the row back. The line a step drew is scratch: what the command has to
   say about the step is printed after it, on a line of its own. */
static void clear_step(void) {
    progress_erase_line(stderr);
    (void)fflush(stderr);
}

/* --- hashing --- */

/*
 * The digest, computed here.
 *
 * This used to run `sha256sum --binary <file>` and read the first field, on
 * the same reasoning curl is shelled out to: molto has no crypto and one
 * fewer thing to keep right is worth a dependency. The reasoning held until
 * the file was named the way Windows names files. GNU coreutils escapes a
 * backslash inside a filename and marks the line by starting it with one, so
 *
 *     sha256sum --binary C:\publish\llvm-mingw.tar.gz
 *
 * answers `\aa998685...  C:\\publish\\llvm-mingw.tar.gz`, whose first field
 * is 65 characters and is therefore "not a digest". Every native Windows path
 * took that branch. The bug was in the parsing, but the parsing existed only
 * because the digest was somebody else's to compute.
 *
 * So molto computes it. The implementation is a port of pickup's, which the
 * ecosystem already relied on to verify every toolchain it unpacks.
 */
static void hashing_progress(long long done, long long total, void *context) {
    publish_progress *shown = context;
    if(total <= 0)
        return;

    /* Redrawn only when the whole number of percent changes: a 127 MB archive
       is two thousand chunks, and a line rewritten two thousand times is a
       line that costs more than it tells. */
    const int percent = (int)((done * 100) / total);
    if(percent == shown->percent)
        return;
    shown->percent = percent;
    step_progress("hashing", percent);
}

static bool checksum_of(const char *file, char *out, size_t size) {
    if(size < SHA256_HEX_SIZE) {
        report("the buffer for a digest is too small, which is a bug in molto");
        return false;
    }

    publish_progress shown = {.percent = -1};
    const bool interactive = progress_is_interactive(stderr);
    if(!sha256_file_watched(file, out, interactive ? hashing_progress : NULL, &shown)) {
        fprintf(stderr, "molto: could not read %s to hash it\n", file);
        return false;
    }
    if(interactive)
        clear_step();
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

/*
 * The bytes, by whichever of the two roads is open.
 *
 * Through the registry is the original road and still the right one for
 * anything that fits: one request, one authority, and the Worker verifies the
 * digest as the stream lands. What it cannot do is carry an archive past the
 * request-body cap Cloudflare puts in front of every Worker -- and a toolchain
 * that runs on Windows goes past it, because it has to be gzip (the tar.exe
 * Windows ships opens nothing else) and gzip is about half as dense as zstd on
 * an LLVM tree. The first one measured was 127.3 MB against 58.6 MB for the
 * same compiler on Linux, and it came back 413 from a proxy, having spent the
 * whole upload to find out.
 *
 * So the signed road is asked for first. The registry checks everything it
 * would have checked -- the coordinate is free, the name is the caller's, the
 * digest is well formed -- and answers a URL that object storage will accept
 * directly. Nothing about the guarantee moves: the digest is part of the
 * signature, storage refuses a stream that does not match it, and the recipe
 * request still measures what landed.
 *
 * A registry that cannot sign says so with a 501, which is not a failure. Every
 * deployment could carry a blob before any of them could sign one, so the
 * fallback is the older road and not an error message.
 */
static bool put_through_registry(const credentials *creds, const char *path, const char *archive,
                                 const char *checksum) {
    registry_response response;
    char err[512] = "";
    if(!registry_upload_blob(creds->registry, creds->token, path, archive, checksum, &response, err,
                             sizeof err)) {
        report(err);
        return false;
    }
    if(response.status != 201)
        return refused("upload", &response);
    return true;
}

static bool put_to_signed_url(const registry_signed_upload *signed_upload, const char *archive) {
    registry_response response;
    char err[512] = "";
    if(!registry_put_signed(signed_upload, archive, &response, err, sizeof err)) {
        report(err);
        return false;
    }

    /* Object storage answers 200 to a PUT it accepted. It is not the registry,
       so its refusal is not in the registry's error shape and there is nothing
       to explain in the registry's terms: what it says is what is shown. */
    if(response.status != 200) {
        fprintf(stderr, "molto: object storage refused the archive (%ld): %.300s\n",
                response.status, response.body);
        return false;
    }
    return true;
}

static bool upload(const credentials *creds, const coordinate *at, const char *archive,
                   const char *checksum) {
    char path[PATH_MAX_LEN];
    if(!fs_format_path(path, sizeof path, "/v1/%ss/%s/%s/%s/blob", at->kind, at->name, at->version,
                       at->target))
        return fs_report_long_path("the upload path");

    char presign_path[PATH_MAX_LEN];
    if(!fs_format_path(presign_path, sizeof presign_path, "%s/presign", path))
        return fs_report_long_path("the signing path");

    registry_signed_upload signed_upload;
    bool signing_available = false;
    char err[512] = "";
    if(!registry_presign_blob(creds->registry, creds->token, presign_path, checksum, &signed_upload,
                              &signing_available, err, sizeof err)) {
        report(err);
        return false;
    }

    fprintf(stderr, "  sending  %s\n",
            signing_available ? "straight to storage, signed by the registry"
                              : "through the registry");
    if(!(signing_available ? put_to_signed_url(&signed_upload, archive)
                           : put_through_registry(creds, path, archive, checksum)))
        return false;

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
                          const char *pack, bool dry_run) {
    if(file != NULL && file[0] != '\0') {
        report("a source recipe has no archive to publish; drop --file");
        return exit_usage_error;
    }
    if(pack != NULL && pack[0] != '\0') {
        report("a source recipe is the whole artifact; there is nothing to pack");
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

/*
 * Making the archive, when there is a directory to make it from.
 *
 * The packing is the recipe's to declare and the target's to default, which is
 * the same order the registry reads them in -- so a recipe that says
 * `format = "tar.gz"` is packed as one, and a windows-* target with no format
 * key is packed as one anyway, because that is the only thing the tar Windows
 * ships can open.
 *
 * Written beside the recipe under the name pickup looks for, so the publish
 * that follows finds it the way it finds any other archive.
 */
static bool pack_beside_recipe(const coordinate *at, const char *recipe_path, const char *directory,
                               char *archive, size_t size) {
    const char *format = at->format[0] != '\0' ? at->format : pack_default_format(at->target);
    if(!pack_format_is_known(format)) {
        fprintf(stderr, "molto: the recipe declares format = \"%s\", which molto cannot pack\n",
                format);
        return false;
    }

    char dir[PATH_MAX_LEN];
    directory_of(recipe_path, dir, sizeof dir);

    char name[COORDINATE_MAX * 4];
    if(!pack_archive_name(at->name, at->version, at->target, format, name, sizeof name))
        return fs_report_long_path("the archive name");
    if(!fs_format_path(archive, size, "%s/%s", dir, name))
        return fs_report_long_path("the archive path");

    fprintf(stderr, "  packing  %s as %s\n", directory, format);

    char err[256] = "";
    if(!pack_directory(directory, archive, format, err, sizeof err)) {
        report(err);
        return false;
    }

    fprintf(stderr, "  packed   %s\n", name);
    if(!pack_is_reproducible())
        fprintf(stderr,
                "  note     this tar cannot sort its entries, so packing again may not\n"
                "           give the same bytes; the digest still describes what was packed\n");
    return true;
}

static int publish_binary(const coordinate *at, const char *recipe_path, const char *file,
                          const char *pack, bool dry_run) {
    char archive[PATH_MAX_LEN];
    if(pack != NULL && pack[0] != '\0') {
        /* Naming both says two different things about which bytes to publish,
           and guessing which one was meant is how the wrong ones go up under a
           coordinate that cannot be republished. */
        if(file != NULL && file[0] != '\0') {
            report("--pack makes the archive and --file names one; use one or the other");
            return exit_usage_error;
        }
        if(!pack_beside_recipe(at, recipe_path, pack, archive, sizeof archive))
            return exit_build_failure;
    } else if(file != NULL && file[0] != '\0')
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

int publish_command_run(const char *recipe, const char *file, const char *pack, bool dry_run) {
    const char *recipe_path = recipe != NULL && recipe[0] != '\0' ? recipe : DEFAULT_RECIPE;

    coordinate at = {0};
    if(!read_coordinate(recipe_path, &at))
        return exit_invalid_manifest;

    const int code = at.from_source ? publish_source(&at, recipe_path, file, pack, dry_run)
                                    : publish_binary(&at, recipe_path, file, pack, dry_run);
    if(code != exit_ok || dry_run)
        return code;

    fprintf(stderr, "Published %s %s@%s (%s)\n", at.kind, at.name, at.version, at.target);
    return exit_ok;
}
