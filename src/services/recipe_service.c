#include <molto/services/recipe_service.h>

#include <molto/services/fs_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARTIFACTS_SECTION "artifacts"

/* What a package compiles itself with and nobody else sees. A dotted path in
   both encodings: a TOML `[artifacts.private]` header and a JSON object nested
   inside `artifacts` read back the same way. */
#define ARTIFACTS_PRIVATE_SECTION "artifacts.private"

/* The build system a source recipe's own sources need (RFC-0009). */
#define BUILD_SECTION "build"

/* Where a plugin declares what it does and what it needs (RFC-0014). */
#define PLUGIN_SECTION "plugin"

/* The document's own top level, where a recipe's coordinate lives. */
#define ROOT_SECTION ""

static bool set_error(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool set_error(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

/* --- the coordinate --- */

static bool read_required(doc_view doc, const char *key, char *out, size_t size, char *err,
                          size_t err_size) {
    if(!doc_get_string(doc, ROOT_SECTION, key, out, size))
        return set_error(err, err_size, "the recipe has no '%s'", key);
    return true;
}

static bool read_schema(doc_view doc, long *out, char *err, size_t err_size) {
    /* Absent means 1: the key is new, and the recipes published before it
       existed cannot be made to declare it retroactively. */
    if(!doc_get_int(doc, ROOT_SECTION, "schema", out)) {
        if(doc_has_key(doc, ROOT_SECTION, "schema"))
            return set_error(err, err_size, "the recipe's 'schema' must be a positive integer");
        *out = 1;
        return true;
    }
    if(*out < 1)
        return set_error(err, err_size, "the recipe's 'schema' must be a positive integer");
    if(*out > RECIPE_SCHEMA_MAX)
        return set_error(err, err_size,
                         "this molto reads recipe schema %d, and the recipe declares %ld; upgrade "
                         "molto",
                         RECIPE_SCHEMA_MAX, *out);
    return true;
}

/* Declared rather than inferred from which tables are present: a source recipe
   with a misspelled [souce] would, by inference, be a perfectly valid binary
   one whose archive merely went missing. */
static bool read_form(doc_view doc, recipe_form *out, char *err, size_t err_size) {
    char form[32];
    if(!doc_get_string(doc, ROOT_SECTION, "form", form, sizeof form)) {
        if(doc_has_key(doc, ROOT_SECTION, "form"))
            return set_error(err, err_size, "the recipe's 'form' must be a string");
        *out = recipe_form_binary;
        return true;
    }
    if(strcmp(form, "binary") == 0)
        *out = recipe_form_binary;
    else if(strcmp(form, "source") == 0)
        *out = recipe_form_source;
    else if(strcmp(form, "platform") == 0)
        *out = recipe_form_platform;
    else
        return set_error(err, err_size, "unknown recipe form '%s'", form);
    return true;
}

bool recipe_read_coordinate(doc_view doc, recipe_coordinate *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);

    if(!read_schema(doc, &out->schema, err, err_size) ||
       !read_form(doc, &out->form, err, err_size) ||
       !read_required(doc, "kind", out->kind, sizeof out->kind, err, err_size) ||
       !read_required(doc, "name", out->name, sizeof out->name, err, err_size) ||
       !read_required(doc, "version", out->version, sizeof out->version, err, err_size) ||
       !read_required(doc, "target", out->target, sizeof out->target, err, err_size))
        return false;

    /* The schema is what makes an older molto refuse the form instead of
       reading a recipe with no [source] as a binary whose upload failed, so
       a platform recipe that does not declare it is the hazard it exists to
       prevent (RFC-0022). */
    if(out->form == recipe_form_platform && out->schema < RECIPE_SCHEMA_PLATFORM)
        return set_error(err, err_size,
                         "a platform recipe must declare schema %d or later, so a molto that "
                         "does not know the form refuses it rather than misreading it",
                         RECIPE_SCHEMA_PLATFORM);
    if(out->form == recipe_form_platform && strcmp(out->kind, "package") != 0)
        return set_error(err, err_size, "only a package may take form = \"platform\"");
    return true;
}

/* --- [artifacts] --- */

static const struct {
    const char *name;
    recipe_artifact_type type;
} ARTIFACT_TYPES[] = {
    {"source", recipe_artifact_source},
    {"static", recipe_artifact_static},
    {"shared", recipe_artifact_shared},
};

static bool read_type(doc_view doc, recipe_artifact_type *out, char *err, size_t err_size) {
    char name[32];
    if(!doc_get_string(doc, ARTIFACTS_SECTION, "type", name, sizeof name)) {
        if(doc_has_key(doc, ARTIFACTS_SECTION, "type"))
            return set_error(err, err_size, "[artifacts].type must be a string");
        return true; /* the default is already seeded */
    }

    for(size_t i = 0; i < sizeof ARTIFACT_TYPES / sizeof ARTIFACT_TYPES[0]; i++) {
        if(strcmp(ARTIFACT_TYPES[i].name, name) == 0) {
            *out = ARTIFACT_TYPES[i].type;
            return true;
        }
    }
    return set_error(err, err_size, "[artifacts].type '%s' is not source, static or shared", name);
}

/*
 * The language standards molto will put behind `-std=`.
 *
 * Checked here rather than left to the compiler because of who writes a recipe
 * and who reads it. A misspelled `[target].std` in a manifest fails in the
 * build of the person who typed it, which is the shortest feedback loop there
 * is; a misspelled `std` in a published recipe fails in the build of everyone
 * who depends on it, and the message names a compiler option none of them
 * wrote. So the manifest is taken at its word and a recipe is not.
 *
 * Two lists rather than one, so a `c++20` written under `std` is caught as
 * well: it is a real standard and still the wrong answer to that key.
 */
static const char *const C_STANDARDS[] = {
    "c89",   "c90",   "c94",   "c99",   "c11",   "c17",   "c18",   "c23",   "c2x",
    "gnu89", "gnu90", "gnu99", "gnu11", "gnu17", "gnu18", "gnu23", "gnu2x",
};

static const char *const CPP_STANDARDS[] = {
    "c++98",   "c++03",   "c++11",   "c++14",   "c++17",   "c++20",   "c++23",
    "c++26",   "c++2a",   "c++2b",   "c++2c",   "gnu++98", "gnu++03", "gnu++11",
    "gnu++14", "gnu++17", "gnu++20", "gnu++23", "gnu++26",
};

/* One standard, against the list for its language. An absent key leaves `out`
   alone, and `out` starts empty — which is what says "whatever the consumer
   compiles with". */
static bool read_std(doc_view doc, const char *key, const char *const *known, size_t count,
                     char *out, size_t out_size, char *err, size_t err_size) {
    /* Read into more room than a standard needs, so an overlong value is
       reported as what was written rather than as a truncation of it. */
    char value[RECIPE_STD_MAX * 2];
    if(!doc_get_string(doc, ARTIFACTS_SECTION, key, value, sizeof value)) {
        if(doc_has_key(doc, ARTIFACTS_SECTION, key))
            return set_error(err, err_size, "[artifacts].%s must be a string", key);
        return true;
    }

    for(size_t i = 0; i < count; i++) {
        if(strcmp(known[i], value) == 0) {
            /* The match is what proves this fits — every entry in `known` is a
               standard's name, and `out` is sized for one. The bound is spelled
               out anyway because the reader of this line cannot see the table
               from here, and neither can the compiler. */
            snprintf(out, out_size, "%.*s", (int)(out_size - 1), value);
            return true;
        }
    }
    return set_error(err, err_size,
                     "[artifacts].%s '%s' is not a language standard molto knows about", key,
                     value);
}

/* The three option lists a table carries. `[artifacts]` and
   `[artifacts.private]` hold the same ones: what separates them is who they
   reach, not what they can say. */
static bool read_options(doc_view doc, const char *table, project_options *out, char *err,
                         size_t err_size) {
    return doc_read_strings(doc, table, "defines", out->defines[0], PROJECT_MAX_OPTS,
                            PROJECT_OPT_LEN, &out->define_count, err, err_size) &&
           doc_read_strings(doc, table, "include", out->include[0], PROJECT_MAX_OPTS,
                            PROJECT_OPT_LEN, &out->include_count, err, err_size) &&
           doc_read_strings(doc, table, "flags", out->flags[0], PROJECT_MAX_OPTS, PROJECT_OPT_LEN,
                            &out->flag_count, err, err_size);
}

/* The names a per-OS table is spelled with, in recipe_os order. */
static const char *const OS_NAMES[RECIPE_OS_COUNT] = {"linux", "macos", "windows"};

/* `[artifacts.<os>]` and `[artifacts.private.<os>]` for one OS. The keys are
   `[artifacts]`'s lists, and only those: a scalar like `type` or `std` cannot
   be appended to, and RFC-0003 gives no way to say one differs by platform. */
static bool read_os_table(doc_view doc, const char *os, recipe_os_artifacts *out, char *err,
                          size_t err_size) {
    char table[64];
    char private_table[64];
    snprintf(table, sizeof table, ARTIFACTS_SECTION ".%s", os);
    snprintf(private_table, sizeof private_table, ARTIFACTS_PRIVATE_SECTION ".%s", os);
    return doc_read_strings(doc, table, "sources", out->sources[0], RECIPE_OS_MAX_SOURCES,
                            RECIPE_SOURCE_MAX, &out->source_count, err, err_size) &&
           doc_read_strings(doc, table, "exclude", out->exclude[0], RECIPE_OS_MAX_SOURCES,
                            RECIPE_SOURCE_MAX, &out->exclude_count, err, err_size) &&
           doc_read_strings(doc, table, "link", out->link[0], PROJECT_MAX_LINK,
                            PROJECT_LINK_NAME_MAX, &out->link_count, err, err_size) &&
           read_options(doc, table, &out->options, err, err_size) &&
           read_options(doc, private_table, &out->private_options, err, err_size);
}

static bool read_per_os(doc_view doc, recipe_artifacts *out, char *err, size_t err_size) {
    for(size_t i = 0; i < RECIPE_OS_COUNT; i++) {
        char table[64];
        char private_table[64];
        snprintf(table, sizeof table, ARTIFACTS_SECTION ".%s", OS_NAMES[i]);
        snprintf(private_table, sizeof private_table, ARTIFACTS_PRIVATE_SECTION ".%s", OS_NAMES[i]);
        if(!doc_has_table(doc, table) && !doc_has_table(doc, private_table))
            continue;

        /* Refused below the schema that defines it, because the reader that
           predates it would not refuse: it would ignore the table and build
           without whatever the table added (RFC-0009). */
        long schema = 1;
        if(!read_schema(doc, &schema, err, err_size))
            return false;
        if(schema < RECIPE_SCHEMA_PER_OS)
            return set_error(err, err_size,
                             "[artifacts.%s] needs schema %d or later, so a molto that does not "
                             "know the table refuses the recipe rather than building without it",
                             OS_NAMES[i], RECIPE_SCHEMA_PER_OS);

        recipe_os_artifacts *os = &out->per_os[i];
        if(!read_os_table(doc, OS_NAMES[i], os, err, err_size))
            return false;
        /* An empty `sources` means every file; appending to it would turn
           "everything" into "only these" on one platform, which is the
           opposite of adding. */
        if(os->source_count > 0 && out->source_count == 0)
            return set_error(err, err_size,
                             "[artifacts.%s].sources adds to [artifacts].sources, which lists "
                             "none and so already means every file; use [artifacts.<os>].exclude "
                             "on the platforms that must not compile them",
                             OS_NAMES[i]);
    }
    return true;
}

static bool has_any_table(doc_view doc) {
    if(doc_has_table(doc, ARTIFACTS_SECTION) || doc_has_table(doc, ARTIFACTS_PRIVATE_SECTION))
        return true;
    for(size_t i = 0; i < RECIPE_OS_COUNT; i++) {
        char table[64];
        char private_table[64];
        snprintf(table, sizeof table, ARTIFACTS_SECTION ".%s", OS_NAMES[i]);
        snprintf(private_table, sizeof private_table, ARTIFACTS_PRIVATE_SECTION ".%s", OS_NAMES[i]);
        if(doc_has_table(doc, table) || doc_has_table(doc, private_table))
            return true;
    }
    return false;
}

bool recipe_read_artifacts(doc_view doc, recipe_artifacts *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    /* RFC-0009's default, seeded before reading so an absent key keeps it. */
    out->type = recipe_artifact_static;

    /* Any table on its own is enough. A recipe whose only statement is a
       private flag declares nothing directly under `[artifacts]`, and asking
       about that table alone would skip the whole read in silence. */
    if(!has_any_table(doc))
        return true;

    return read_type(doc, &out->type, err, err_size) &&
           read_std(doc, "std", C_STANDARDS, sizeof C_STANDARDS / sizeof C_STANDARDS[0], out->std,
                    sizeof out->std, err, err_size) &&
           read_std(doc, "cpp_std", CPP_STANDARDS, sizeof CPP_STANDARDS / sizeof CPP_STANDARDS[0],
                    out->cpp_std, sizeof out->cpp_std, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "sources", out->sources[0], RECIPE_MAX_SOURCES,
                            RECIPE_SOURCE_MAX, &out->source_count, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "exclude", out->exclude[0], RECIPE_MAX_SOURCES,
                            RECIPE_SOURCE_MAX, &out->exclude_count, err, err_size) &&
           doc_read_strings(doc, ARTIFACTS_SECTION, "link", out->link[0], PROJECT_MAX_LINK,
                            PROJECT_LINK_NAME_MAX, &out->link_count, err, err_size) &&
           read_options(doc, ARTIFACTS_SECTION, &out->options, err, err_size) &&
           read_options(doc, ARTIFACTS_PRIVATE_SECTION, &out->private_options, err, err_size) &&
           read_per_os(doc, out, err, err_size);
}

/* --- [[provide]] --- */

#define PROVIDE_ARRAY "provide"

/* One entry. Both keys are required: an entry naming only one of them says half
   a thing, and guessing the other half is how a recipe comes to mean something
   nobody wrote. */
static bool read_provision(doc_view item, size_t index, recipe_provision *out, char *err,
                           size_t err_size) {
    if(!doc_get_string(item, "", "file", out->file, sizeof out->file))
        return set_error(err, err_size, "[[provide]] #%zu has no 'file'", index + 1);
    if(!doc_get_string(item, "", "from", out->from, sizeof out->from))
        return set_error(err, err_size, "[[provide]] #%zu has no 'from'", index + 1);
    if(out->file[0] == '\0' || out->from[0] == '\0')
        return set_error(err, err_size, "[[provide]] #%zu names an empty path", index + 1);
    return true;
}

bool recipe_read_provide(doc_view doc, recipe_provide *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);

    const size_t count = doc_array_len(doc, PROVIDE_ARRAY);
    if(count == 0)
        return true;
    /* Reported rather than truncated, like every other list in this file: an
       entry silently dropped is a header that is not written and a compiler
       error that names neither the recipe nor the key. */
    if(count > RECIPE_MAX_PROVIDE)
        return set_error(err, err_size,
                         "[[provide]] has %zu entries and at most %d may be declared; a recipe "
                         "needing more is restructuring a source tree rather than completing its "
                         "configuration",
                         count, RECIPE_MAX_PROVIDE);

    for(size_t i = 0; i < count; i++) {
        doc_view item;
        if(!doc_array_at(doc, PROVIDE_ARRAY, i, &item))
            return set_error(err, err_size, "[[provide]] #%zu is not a table", i + 1);
        if(!read_provision(item, i, &out->items[i], err, err_size))
            return false;
    }
    out->count = count;
    return true;
}

/* --- [build] --- */

static const struct {
    const char *name;
    recipe_build_system system;
} BUILD_SYSTEMS[] = {
    {"none", recipe_build_none},   {"make", recipe_build_make},
    {"cmake", recipe_build_cmake}, {"autotools", recipe_build_autotools},
    {"meson", recipe_build_meson},
};

const char *recipe_build_system_name(recipe_build_system system) {
    for(size_t i = 0; i < sizeof BUILD_SYSTEMS / sizeof BUILD_SYSTEMS[0]; i++) {
        if(BUILD_SYSTEMS[i].system == system)
            return BUILD_SYSTEMS[i].name;
    }
    return "none";
}

bool recipe_read_build(doc_view doc, recipe_build *out, char *err, size_t err_size) {
    /* `recipe_build_none` is zero, so this is also the absent answer. */
    memset(out, 0, sizeof *out);

    char name[32];
    if(!doc_get_string(doc, BUILD_SECTION, "system", name, sizeof name)) {
        if(doc_has_key(doc, BUILD_SECTION, "system"))
            return set_error(err, err_size, "[build].system must be a string");
        return true;
    }

    for(size_t i = 0; i < sizeof BUILD_SYSTEMS / sizeof BUILD_SYSTEMS[0]; i++) {
        if(strcmp(BUILD_SYSTEMS[i].name, name) == 0) {
            out->system = BUILD_SYSTEMS[i].system;
            return true;
        }
    }
    /* Never a fallback to running it anyway, and never a fallback to `none`:
       one would be `sh -c` on a stranger's word and the other would compile
       sources that were told they needed configuring first (RFC-0009). */
    return set_error(err, err_size,
                     "[build].system '%s' is not none, make, cmake, autotools or meson", name);
}

/* --- [plugin] --- */

bool recipe_read_plugin(doc_view doc, recipe_plugin *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);

    if(!doc_has_table(doc, PLUGIN_SECTION))
        return set_error(err, err_size, "the recipe has no [plugin] table");

    /* Checked before a single key of the table is read, because what it
       protects is a molto that never reads the table at all. `[plugin]` is what
       schema 2 added; a recipe carrying it while declaring schema 1 is one an
       older molto accepts, skips the table it does not recognise, and installs
       — an executable whose permissions nobody was shown. Declaring the schema
       is exactly what turns that into a refusal over there, and refusing it
       here is what stops such a recipe being written in the first place. */
    long schema = 0;
    if(!read_schema(doc, &schema, err, err_size))
        return false;
    if(schema < RECIPE_SCHEMA_PLUGIN)
        return set_error(err, err_size,
                         "the recipe declares schema %ld and carries a [plugin] table, which is "
                         "schema %d: a molto predating plugins would read this recipe, ignore the "
                         "table and install the binary without showing what it asks for",
                         schema, RECIPE_SCHEMA_PLUGIN);

    if(!doc_read_strings(doc, PLUGIN_SECTION, "capabilities", out->capabilities[0],
                         RECIPE_PLUGIN_MAX_CAPABILITIES, RECIPE_PLUGIN_ENTRY_MAX,
                         &out->capability_count, err, err_size))
        return false;
    if(out->capability_count == 0)
        return set_error(err, err_size, "[plugin].capabilities names none");

    if(!doc_read_strings(doc, PLUGIN_SECTION, "extensions", out->extensions[0],
                         RECIPE_PLUGIN_MAX_EXTENSIONS, RECIPE_PLUGIN_ENTRY_MAX,
                         &out->extension_count, err, err_size))
        return false;
    if(!doc_read_strings(doc, PLUGIN_SECTION, "permissions", out->permissions[0],
                         RECIPE_PLUGIN_MAX_PERMISSIONS, RECIPE_PLUGIN_ENTRY_MAX,
                         &out->permission_count, err, err_size))
        return false;

    /* Both are optional and both are reported as absent rather than defaulted:
       a plugin that names no IR schema has not agreed to one, and guessing on
       its behalf is how a version mismatch turns into a half-read document. */
    if(!doc_get_int(doc, PLUGIN_SECTION, "ir_schema", &out->ir_schema) &&
       doc_has_key(doc, PLUGIN_SECTION, "ir_schema"))
        return set_error(err, err_size, "[plugin].ir_schema must be an integer");
    if(!doc_get_string(doc, PLUGIN_SECTION, "molto_min", out->molto_min, sizeof out->molto_min) &&
       doc_has_key(doc, PLUGIN_SECTION, "molto_min"))
        return set_error(err, err_size, "[plugin].molto_min must be a string");

    return true;
}

static bool listed_in(const char list[][RECIPE_SOURCE_MAX], size_t count, const char *name) {
    for(size_t i = 0; i < count; i++) {
        if(strcmp(list[i], name) == 0)
            return true;
    }
    return false;
}

bool recipe_artifacts_wants(const recipe_artifacts *artifacts, const char *name) {
    /* An empty `sources` means every source the drop happens to contain, which
       is what a recipe that never needed to narrow anything says by omission. */
    if(artifacts->source_count > 0 && !listed_in(artifacts->sources, artifacts->source_count, name))
        return false;
    return !listed_in(artifacts->exclude, artifacts->exclude_count, name);
}

/* --- the build's OS --- */

recipe_os recipe_os_for_platform(const char *platform) {
    if(platform == NULL || platform[0] == '\0') {
#if defined(_WIN32)
        return recipe_os_windows;
#elif defined(__APPLE__)
        return recipe_os_macos;
#elif defined(__linux__)
        return recipe_os_linux;
#else
        return recipe_os_none;
#endif
    }
    /* The spellings a triple uses for each: `x86_64-w64-mingw32` and
       `x86_64-pc-windows-msvc`, `aarch64-apple-darwin`, `x86_64-linux-gnu`. */
    if(strstr(platform, "windows") != NULL || strstr(platform, "mingw") != NULL ||
       strstr(platform, "-w64-") != NULL)
        return recipe_os_windows;
    if(strstr(platform, "darwin") != NULL || strstr(platform, "apple") != NULL ||
       strstr(platform, "macos") != NULL)
        return recipe_os_macos;
    if(strstr(platform, "linux") != NULL)
        return recipe_os_linux;
    return recipe_os_none;
}

/* `count` entries of `from` onto the end of `to`, which holds `*to_count` of
   `capacity`. Overflow names the key, since the list that overflowed is the
   merge of two tables and neither alone is too long. */
static bool append_strings(char *to, size_t *to_count, size_t capacity, size_t width,
                           const char *from, size_t count, const char *os, const char *key,
                           char *err, size_t err_size) {
    if(*to_count + count > capacity)
        return set_error(err, err_size,
                         "[artifacts].%s and [artifacts.%s].%s together hold more than %zu entries",
                         key, os, key, capacity);
    for(size_t i = 0; i < count; i++)
        snprintf(to + (*to_count + i) * width, width, "%s", from + i * width);
    *to_count += count;
    return true;
}

static bool append_options(project_options *to, const project_options *from, const char *os,
                           char *err, size_t err_size) {
    return append_strings(to->defines[0], &to->define_count, PROJECT_MAX_OPTS, PROJECT_OPT_LEN,
                          from->defines[0], from->define_count, os, "defines", err, err_size) &&
           append_strings(to->include[0], &to->include_count, PROJECT_MAX_OPTS, PROJECT_OPT_LEN,
                          from->include[0], from->include_count, os, "include", err, err_size) &&
           append_strings(to->flags[0], &to->flag_count, PROJECT_MAX_OPTS, PROJECT_OPT_LEN,
                          from->flags[0], from->flag_count, os, "flags", err, err_size);
}

bool recipe_artifacts_select_os(recipe_artifacts *artifacts, recipe_os os, char *err,
                                size_t err_size) {
    if(artifacts->os_selected)
        return true;
    bool ok = true;
    if(os != recipe_os_none) {
        const recipe_os_artifacts *add = &artifacts->per_os[os];
        const char *name = OS_NAMES[os];
        ok =
            append_strings(artifacts->sources[0], &artifacts->source_count, RECIPE_MAX_SOURCES,
                           RECIPE_SOURCE_MAX, add->sources[0], add->source_count, name, "sources",
                           err, err_size) &&
            append_strings(artifacts->exclude[0], &artifacts->exclude_count, RECIPE_MAX_SOURCES,
                           RECIPE_SOURCE_MAX, add->exclude[0], add->exclude_count, name, "exclude",
                           err, err_size) &&
            append_strings(artifacts->link[0], &artifacts->link_count, PROJECT_MAX_LINK,
                           PROJECT_LINK_NAME_MAX, add->link[0], add->link_count, name, "link", err,
                           err_size) &&
            append_options(&artifacts->options, &add->options, name, err, err_size) &&
            append_options(&artifacts->private_options, &add->private_options, name, err, err_size);
    }
    memset(artifacts->per_os, 0, sizeof artifacts->per_os);
    artifacts->os_selected = true;
    return ok;
}

/* --- [overlay] --- */

#define OVERLAY_SECTION "overlay"
#define OVERLAY_FILES OVERLAY_SECTION ".files"

bool recipe_overlay_path_ok(const char *path) {
    if(path[0] == '\0' || path[0] == '/' || strchr(path, '\\') != NULL || strchr(path, ':') != NULL)
        return false;
    /* Component by component, so `a..b` is a name and `a/../b` is not. */
    for(const char *part = path; *part != '\0';) {
        const char *end = strchr(part, '/');
        const size_t length = end != NULL ? (size_t)(end - part) : strlen(part);
        if(length == 0 || (length == 1 && part[0] == '.') ||
           (length == 2 && part[0] == '.' && part[1] == '.'))
            return false;
        part += length + (end != NULL ? 1 : 0);
    }
    return true;
}

/* One published file, written where the overlay's directory says. Written
   once: a coordinate's bytes never change, so a file already there with the
   same content is what an earlier build left, and one with other content is
   a cache someone edited — reported rather than trusted or overwritten. */
static bool materialize_file(const char *dir, const char *path, const char *content, char *err,
                             size_t err_size) {
    char file[RECIPE_OVERLAY_DIR_MAX + RECIPE_PROVIDE_PATH_MAX];
    if(!fs_format_path(file, sizeof file, "%s/%s", dir, path))
        return set_error(err, err_size, "[overlay] file '%s' makes a path that is too long", path);
    if(fs_path_exists(file)) {
        char *existing = fs_read_file(file);
        const bool same = existing != NULL && strcmp(existing, content) == 0;
        free(existing);
        if(same)
            return true;
        return set_error(err, err_size,
                         "[overlay] file '%s' is already in molto's cache at %s with other bytes",
                         path, file);
    }
    char parent[sizeof file];
    snprintf(parent, sizeof parent, "%s", file);
    char *slash = strrchr(parent, '/');
    if(slash != NULL)
        *slash = '\0';
    if(!fs_make_dirs(parent) || !fs_write_file(file, content))
        return set_error(err, err_size, "could not write [overlay] file '%s' to %s", path, file);
    return true;
}

static bool read_overlay_files(doc_view doc, const char *materialize_dir, recipe_overlay *out,
                               char *err, size_t err_size) {
    if(materialize_dir == NULL)
        return set_error(err, err_size,
                         "[overlay].files belongs to a published recipe; one on disk names its "
                         "directory with [overlay].path");
    const size_t count = doc_array_len(doc, OVERLAY_FILES);
    if(count == 0)
        return set_error(err, err_size, "[overlay].files lists no file");
    if(count > RECIPE_OVERLAY_MAX_FILES)
        return set_error(err, err_size, "[overlay].files has %zu files and at most %d may be",
                         count, RECIPE_OVERLAY_MAX_FILES);

    char *content = malloc(RECIPE_OVERLAY_MAX_BYTES + 1);
    if(content == NULL)
        return set_error(err, err_size, "out of memory reading [overlay]");
    size_t total = 0;
    bool ok = true;
    for(size_t i = 0; ok && i < count; i++) {
        doc_view item;
        char path[RECIPE_PROVIDE_PATH_MAX];
        if(!doc_array_at(doc, OVERLAY_FILES, i, &item) ||
           !doc_get_string(item, "", "path", path, sizeof path) ||
           !doc_get_string(item, "", "content", content, RECIPE_OVERLAY_MAX_BYTES + 1)) {
            ok = set_error(err, err_size, "[overlay].files #%zu needs a 'path' and a 'content'",
                           i + 1);
            break;
        }
        total += strlen(content);
        if(!recipe_overlay_path_ok(path))
            ok = set_error(err, err_size,
                           "[overlay].files #%zu names '%s', which is not a relative path inside "
                           "the source",
                           i + 1, path);
        else if(total > RECIPE_OVERLAY_MAX_BYTES)
            ok = set_error(err, err_size, "[overlay].files hold more than %d bytes",
                           RECIPE_OVERLAY_MAX_BYTES);
        else
            ok = materialize_file(materialize_dir, path, content, err, err_size);
    }
    free(content);
    if(ok)
        snprintf(out->dir, sizeof out->dir, "%s", materialize_dir);
    return ok;
}

static bool read_overlay_path(doc_view doc, const char *recipe_dir, recipe_overlay *out, char *err,
                              size_t err_size) {
    char path[RECIPE_PROVIDE_PATH_MAX];
    if(!doc_get_string(doc, OVERLAY_SECTION, "path", path, sizeof path))
        return set_error(err, err_size, "[overlay].path must be a string");
    if(!recipe_overlay_path_ok(path))
        return set_error(err, err_size, "[overlay].path '%s' is not a directory beside the recipe",
                         path);
    if(recipe_dir == NULL)
        return set_error(err, err_size,
                         "[overlay].path names a directory beside the recipe, and this recipe was "
                         "not read from one");
    if(!fs_format_path(out->dir, sizeof out->dir, "%s/%s", recipe_dir, path) ||
       !fs_is_dir(out->dir))
        return set_error(err, err_size, "[overlay].path '%s' is not a directory beside the recipe",
                         path);
    return true;
}

bool recipe_read_overlay(doc_view doc, const char *recipe_dir, const char *materialize_dir,
                         recipe_overlay *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    if(!doc_has_table(doc, OVERLAY_SECTION))
        return true;

    long schema = 1;
    if(!read_schema(doc, &schema, err, err_size))
        return false;
    if(schema < RECIPE_SCHEMA_OVERLAY)
        return set_error(err, err_size,
                         "[overlay] needs schema %d or later, so a molto that does not know the "
                         "table refuses the recipe rather than compiling without its files",
                         RECIPE_SCHEMA_OVERLAY);

    const bool has_path = doc_has_key(doc, OVERLAY_SECTION, "path");
    const bool has_files = doc_has_key(doc, OVERLAY_SECTION, "files");
    if(has_path == has_files)
        return set_error(err, err_size,
                         "[overlay] takes either 'path' (a recipe on disk) or 'files' (a "
                         "published one)");
    out->present = true;
    return has_path ? read_overlay_path(doc, recipe_dir, out, err, err_size)
                    : read_overlay_files(doc, materialize_dir, out, err, err_size);
}
