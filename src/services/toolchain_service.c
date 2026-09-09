#include <molto/services/toolchain_service.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/util/str_list.h>
#include <molto/util/toml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The resolver Molto asks, and how to find it. */
#define PICKUP_PROGRAM "pickup"
#define PICKUP_PATH_ENV "MOLTO_PICKUP" /* explicit path, for an uninstalled build */

/* Manual overrides. Setting these bypasses resolution entirely. */
#define CC_OVERRIDE_ENV "C_COMPILER"
#define CXX_OVERRIDE_ENV "CPP_COMPILER"

/* The pickup sub-command and its arguments. */
#define ARG_RESOLVE "resolve"
#define ARG_TARGET "--target"
#define ARG_HOST "host"
#define ARG_LANG "--lang"
#define LANG_C "c"
#define LANG_CXX "c++"
#define ARG_STD "--std"
#define ARG_REQUIRE "--require"
#define ARG_VENDOR "--vendor"
#define ARG_FORMAT "--format"
#define FORMAT_TOML "toml"

/* Exit code pickup uses for "nothing satisfies the request" (its spec.md
   section 12). Distinct from a malfunction, and reported differently. */
#define PICKUP_EXIT_NO_MATCH 3

/* Keys of the TOML pickup answers with. */
#define ANSWER_SECTION "compiler"
#define ANSWER_PATH "path"
#define ANSWER_C_PATH "c_path"
#define ANSWER_CXX_PATH "cxx_path"
#define ANSWER_VENDOR "vendor"
#define ANSWER_VERSION "version"
#define ANSWER_ID "id"
#define ANSWER_TARGET "target"
#define ANSWER_STD_FLAG "std_flag"

/* The link recipe sits in its own section, named for the language the request
   asked about rather than under a fixed name: pickup writes [c] for a C
   resolution and [cxx] for a C++ one, and reading the wrong one finds nothing
   at all. */
#define RECIPE_SECTION_C "c"
#define RECIPE_SECTION_CXX "cxx"
#define RECIPE_STDLIB "stdlib"
#define RECIPE_COMPILE_FLAGS "compile_flags"
#define RECIPE_LINK_FLAGS "link_flags"
#define RECIPE_RUNTIME_DIRS "runtime_dirs"

/* Values written into the workspace database before the three variable-length
   tables: cc, cxx, vendor, version, id, target, std_flag, stdlib. */
#define WSDB_FIXED_VALUES 8

/* The workspace database key holding the resolved C toolchain. */
#define TOOLCHAIN_KEY "toolchain:c"

/* Size of the buffer receiving pickup's answer. */
#define ANSWER_SIZE 8192

/* Size of the buffer holding the serialized request. */
#define REQUEST_SIZE 2048

/* Separator between feature ids in a --require argument. */
#define FEATURE_SEPARATOR ","

/* Map a manifest's `compiler` onto a pickup vendor. The key used to name a
   compiler family bound to fixed binaries; it now expresses a preference, and
   the old spellings keep working. */
static const char *vendor_of(const char *compiler) {
    if(compiler[0] == '\0')
        return NULL; /* no preference: any vendor may answer */
    if(strcmp(compiler, "gcc") == 0 || strcmp(compiler, "g++") == 0)
        return "gcc";
    if(strcmp(compiler, "clang") == 0 || strcmp(compiler, "llvm") == 0)
        return "clang";
    if(strcmp(compiler, "apple-clang") == 0)
        return "apple-clang";
    if(strcmp(compiler, "msvc") == 0)
        return "msvc";
    return compiler; /* validated by the manifest; pass it through */
}

/* Join the required features into one comma-separated argument. */
static bool compose_features(const project_target *target, char *out, size_t out_size) {
    out[0] = '\0';
    size_t used = 0;
    for(size_t i = 0; i < target->requires_count; i++) {
        int written = snprintf(out + used, out_size - used, "%s%s", i > 0 ? FEATURE_SEPARATOR : "",
                               target->requires[i]);
        if(written < 0 || (size_t)written >= out_size - used)
            return false;
        used += (size_t)written;
    }
    return true;
}

/* Serialize the request. This string is both what pickup is asked and the
   fingerprint the answer is stored under, so the two can never disagree. */
static bool compose_request(const project_target *target, const char *platform, bool needs_cpp,
                            const char *features, char *out, size_t out_size) {
    const char *vendor = vendor_of(target->compiler);
    return fs_format_path(out, out_size, "lang=%s std=%s require=%s vendor=%s target=%s",
                          needs_cpp ? LANG_CXX : LANG_C, target->std[0] != '\0' ? target->std : "-",
                          features[0] != '\0' ? features : "-", vendor != NULL ? vendor : "-",
                          platform != NULL ? platform : "-");
}

/* Build the pickup command line for this request. */
static bool build_pickup_argv(const char *program, const project_target *target,
                              const char *platform, bool needs_cpp, const char *features,
                              str_list *argv) {
    bool ok = str_list_push(argv, program) && str_list_push(argv, ARG_RESOLVE) &&
              str_list_push(argv, ARG_LANG) && str_list_push(argv, needs_cpp ? LANG_CXX : LANG_C);
    if(ok && target->std[0] != '\0')
        ok = str_list_push(argv, ARG_STD) && str_list_push(argv, target->std);
    if(ok && features[0] != '\0')
        ok = str_list_push(argv, ARG_REQUIRE) && str_list_push(argv, features);
    const char *vendor = vendor_of(target->compiler);
    if(ok && vendor != NULL)
        ok = str_list_push(argv, ARG_VENDOR) && str_list_push(argv, vendor);
    if(ok && platform != NULL)
        ok = str_list_push(argv, ARG_TARGET) && str_list_push(argv, platform);
    return ok && str_list_push(argv, ARG_FORMAT) && str_list_push(argv, FORMAT_TOML);
}

/* Run a command held in a str_list and capture its stdout. */
static int run_capturing(const str_list *argv, char *out, size_t out_size) {
    size_t count = str_list_count(argv);
    const char **cargv = (const char **)malloc((count + 1) * sizeof(char *));
    if(cargv == NULL)
        return -1;
    for(size_t i = 0; i < count; i++)
        cargv[i] = str_list_get(argv, i);
    cargv[count] = NULL;
    int status = process_capture(cargv, out, out_size);
    free((void *)cargv);
    return status;
}

/* Copy a TOML string array into one of the recipe's fixed tables.
 *
 * `rows` is addressed as bytes because the two tables have different widths — a
 * flag is not as long as a path — and C cannot pass those through one parameter
 * otherwise.
 *
 * An absent key is not a failure: a toolchain that needs no flags answers with
 * an empty array. An array that does not fit *is* one, and is refused loudly.
 * Keeping the first eight of nine flags would produce exactly the quietly-wrong
 * build this path exists to prevent, and these tables are sized to pickup's own
 * limits — so overflowing one means pickup grew and molto has to be told.
 */
static bool take_array(const toml_document *doc, const char *section, const char *key, char *rows,
                       size_t stride, size_t max_rows, size_t *count_out) {
    *count_out = 0;

    str_list values;
    str_list_init(&values);
    if(!toml_get_array(doc, section, key, &values)) {
        str_list_free(&values);
        return true;
    }

    const size_t count = str_list_count(&values);
    bool ok = true;
    if(count > max_rows) {
        fprintf(stderr, "molto: pickup answered %zu entries in [%s] %s and molto carries %zu\n",
                count, section, key, max_rows);
        ok = false;
    }
    for(size_t i = 0; ok && i < count; i++) {
        const char *value = str_list_get(&values, i);
        const size_t length = strlen(value);
        if(length >= stride) {
            fprintf(stderr, "molto: an entry in [%s] %s is too long for molto to carry\n", section,
                    key);
            ok = false;
            break;
        }
        memcpy(rows + i * stride, value, length + 1);
        *count_out = i + 1;
    }
    str_list_free(&values);
    return ok;
}

/* Read pickup's TOML answer. Molto already parses TOML, which is why pickup
   speaks it: consuming the resolver adds no parser here.

   `needs_cpp` picks the recipe section, because it is what picked the `--lang`
   that produced it. */
static bool parse_answer(const char *toml, bool needs_cpp, resolved_toolchain *out) {
    char err[256] = "";
    toml_document *doc = toml_parse(toml, err, sizeof err);
    if(doc == NULL)
        return false;

    /* Cleared first: every field below is optional, so a key pickup did not
       write has to read back empty rather than as whatever the caller's stack
       happened to hold. */
    memset(out, 0, sizeof *out);

    /* c_path is the C driver of the toolchain whatever language was asked
       about; path answers only the language of the request. */
    bool ok = toml_get_string(doc, ANSWER_SECTION, ANSWER_C_PATH, out->cc, sizeof out->cc) ||
              toml_get_string(doc, ANSWER_SECTION, ANSWER_PATH, out->cc, sizeof out->cc);
    /* The rest is descriptive; a missing field leaves it empty rather than
       failing a resolution that already named a compiler. */
    (void)toml_get_string(doc, ANSWER_SECTION, ANSWER_CXX_PATH, out->cxx, sizeof out->cxx);
    (void)toml_get_string(doc, ANSWER_SECTION, ANSWER_VENDOR, out->vendor, sizeof out->vendor);
    (void)toml_get_string(doc, ANSWER_SECTION, ANSWER_VERSION, out->version, sizeof out->version);
    (void)toml_get_string(doc, ANSWER_SECTION, ANSWER_ID, out->id, sizeof out->id);
    (void)toml_get_string(doc, ANSWER_SECTION, ANSWER_TARGET, out->target, sizeof out->target);
    (void)toml_get_string(doc, ANSWER_SECTION, ANSWER_STD_FLAG, out->std_flag,
                          sizeof out->std_flag);

    /* The recipe. Unlike the fields above it is not descriptive: these are the
       terms under which the compiler just named produces a program that runs,
       and dropping them is how a build succeeds and its output will not start. */
    const char *recipe = needs_cpp ? RECIPE_SECTION_CXX : RECIPE_SECTION_C;
    (void)toml_get_string(doc, recipe, RECIPE_STDLIB, out->stdlib, sizeof out->stdlib);
    ok = ok &&
         take_array(doc, recipe, RECIPE_COMPILE_FLAGS, &out->compile_flags[0][0],
                    TOOLCHAIN_FLAG_MAX, TOOLCHAIN_MAX_FLAGS, &out->compile_flag_count) &&
         take_array(doc, recipe, RECIPE_LINK_FLAGS, &out->link_flags[0][0], TOOLCHAIN_FLAG_MAX,
                    TOOLCHAIN_MAX_FLAGS, &out->link_flag_count) &&
         take_array(doc, recipe, RECIPE_RUNTIME_DIRS, &out->runtime_dirs[0][0], TOOLCHAIN_PATH_MAX,
                    TOOLCHAIN_MAX_DIRS, &out->runtime_dir_count);

    toml_free(doc);
    return ok && out->cc[0] != '\0';
}

/* Where pickup lives: an explicit path when given, otherwise the PATH. */
static const char *pickup_program(void) {
    const char *explicit_path = getenv(PICKUP_PATH_ENV);
    if(explicit_path != NULL && explicit_path[0] != '\0')
        return explicit_path;
    return PICKUP_PROGRAM;
}

/* Take the compilers straight from the environment, bypassing resolution.
   Returns false when C_COMPILER is not set, which is the normal case. */
static bool take_from_environment(resolved_toolchain *out) {
    const char *cc = getenv(CC_OVERRIDE_ENV);
    if(cc == NULL || cc[0] == '\0')
        return false;

    memset(out, 0, sizeof *out);
    if(!fs_format_path(out->cc, sizeof out->cc, "%s", cc))
        return false;
    const char *cxx = getenv(CXX_OVERRIDE_ENV);
    if(cxx != NULL && cxx[0] != '\0')
        (void)fs_format_path(out->cxx, sizeof out->cxx, "%s", cxx);

    /* Say so: this compiler was chosen by hand, not proven to meet the
       manifest's requirements. */
    fprintf(stderr, "molto: using " CC_OVERRIDE_ENV "=%s (requirements not checked)\n", out->cc);
    return true;
}

/* --- the answer, as the workspace database holds it ---
 *
 * A flat list of strings, because that is what the database stores: the eight
 * fixed fields in the order below, then each variable-length table as its own
 * count followed by its entries. Self-describing rather than positional past
 * the fixed head, so a table growing an entry does not silently shift the
 * meaning of everything after it.
 *
 * The entries are still read positionally, so any change to this layout has to
 * be accompanied by a WSDB_VERSION bump — an older database is discarded and
 * rebuilt, which costs one resolution and never a misread answer.
 */

/* Append one variable-length table: its count, then its entries. */
static bool push_table(str_list *values, const char *rows, size_t stride, size_t count) {
    char number[24];
    snprintf(number, sizeof number, "%zu", count);
    if(!str_list_push(values, number))
        return false;
    for(size_t i = 0; i < count; i++) {
        if(!str_list_push(values, rows + i * stride))
            return false;
    }
    return true;
}

/* Read one back, advancing `at` past everything it consumed. False on anything
   that does not describe a table this build can hold, which discards the entry
   and asks pickup again rather than building on a half-read answer. */
static bool take_table(const str_list *values, size_t *at, char *rows, size_t stride,
                       size_t max_rows, size_t *count_out) {
    *count_out = 0;

    const char *number = str_list_get(values, *at);
    if(number == NULL)
        return false;
    (*at)++;

    char *end = NULL;
    const unsigned long count = strtoul(number, &end, 10);
    if(end == number || *end != '\0' || count > max_rows)
        return false;

    for(unsigned long i = 0; i < count; i++) {
        const char *value = str_list_get(values, *at);
        if(value == NULL)
            return false;
        const size_t length = strlen(value);
        if(length >= stride)
            return false;
        memcpy(rows + i * stride, value, length + 1);
        (*at)++;
    }
    *count_out = (size_t)count;
    return true;
}

/* Serve the answer recorded in the workspace database. */
static bool take_from_wsdb(wsdb *db, const char *request, resolved_toolchain *out) {
    if(db == NULL || !wsdb_toolchain_fresh(db, TOOLCHAIN_KEY, request))
        return false;

    str_list values;
    str_list_init(&values);
    bool ok = wsdb_toolchain_values(db, TOOLCHAIN_KEY, &values) &&
              str_list_count(&values) >= WSDB_FIXED_VALUES;
    if(ok) {
        memset(out, 0, sizeof *out);
        ok = fs_format_path(out->cc, sizeof out->cc, "%s", str_list_get(&values, 0)) &&
             fs_format_path(out->cxx, sizeof out->cxx, "%s", str_list_get(&values, 1)) &&
             fs_format_path(out->vendor, sizeof out->vendor, "%s", str_list_get(&values, 2)) &&
             fs_format_path(out->version, sizeof out->version, "%s", str_list_get(&values, 3)) &&
             fs_format_path(out->id, sizeof out->id, "%s", str_list_get(&values, 4)) &&
             fs_format_path(out->target, sizeof out->target, "%s", str_list_get(&values, 5)) &&
             fs_format_path(out->std_flag, sizeof out->std_flag, "%s", str_list_get(&values, 6)) &&
             fs_format_path(out->stdlib, sizeof out->stdlib, "%s", str_list_get(&values, 7));
    }
    if(ok) {
        size_t at = WSDB_FIXED_VALUES;
        ok = take_table(&values, &at, &out->compile_flags[0][0], TOOLCHAIN_FLAG_MAX,
                        TOOLCHAIN_MAX_FLAGS, &out->compile_flag_count) &&
             take_table(&values, &at, &out->link_flags[0][0], TOOLCHAIN_FLAG_MAX,
                        TOOLCHAIN_MAX_FLAGS, &out->link_flag_count) &&
             take_table(&values, &at, &out->runtime_dirs[0][0], TOOLCHAIN_PATH_MAX,
                        TOOLCHAIN_MAX_DIRS, &out->runtime_dir_count);
    }
    str_list_free(&values);
    return ok;
}

/* Record an answer so the next build does not have to ask again. */
static void remember(wsdb *db, const char *request, const resolved_toolchain *chain) {
    if(db == NULL)
        return;
    str_list values;
    str_list_init(&values);
    /* The compiler path goes first: the database registers it as an input, so
       replacing the compiler invalidates this entry. */
    bool ok = str_list_push(&values, chain->cc) && str_list_push(&values, chain->cxx) &&
              str_list_push(&values, chain->vendor) && str_list_push(&values, chain->version) &&
              str_list_push(&values, chain->id) && str_list_push(&values, chain->target) &&
              str_list_push(&values, chain->std_flag) && str_list_push(&values, chain->stdlib);
    ok =
        ok &&
        push_table(&values, &chain->compile_flags[0][0], TOOLCHAIN_FLAG_MAX,
                   chain->compile_flag_count) &&
        push_table(&values, &chain->link_flags[0][0], TOOLCHAIN_FLAG_MAX, chain->link_flag_count) &&
        push_table(&values, &chain->runtime_dirs[0][0], TOOLCHAIN_PATH_MAX,
                   chain->runtime_dir_count);
    if(!ok || !wsdb_record_toolchain(db, TOOLCHAIN_KEY, request, &values))
        fprintf(stderr, "molto: warning: could not record the resolved toolchain\n");
    str_list_free(&values);
}

/* Ask pickup, and turn its failures into messages that say what to do. */
static int ask_pickup(const project_target *target, const char *platform, bool needs_cpp,
                      const char *features, resolved_toolchain *out) {
    const char *program = pickup_program();

    str_list argv;
    str_list_init(&argv);
    if(!build_pickup_argv(program, target, platform, needs_cpp, features, &argv)) {
        str_list_free(&argv);
        return exit_build_failure;
    }

    char answer[ANSWER_SIZE];
    int status = run_capturing(&argv, answer, sizeof answer);
    str_list_free(&argv);

    if(status == PICKUP_EXIT_NO_MATCH) {
        /* Pickup already explained what each candidate lacks, on its own
           stderr, which the user has just read. */
        fprintf(stderr, "molto: no compiler here meets [target] (see above)\n");
        return exit_build_failure;
    }
    if(status != 0) {
        fprintf(stderr,
                "molto: could not run '%s' to resolve the compiler.\n"
                "  Install pickup, or point " PICKUP_PATH_ENV " at it,\n"
                "  or set " CC_OVERRIDE_ENV " (and " CXX_OVERRIDE_ENV ") to a compiler.\n",
                program);
        return exit_build_failure;
    }
    if(!parse_answer(answer, needs_cpp, out)) {
        fprintf(stderr, "molto: could not read the answer from '%s'\n", program);
        return exit_build_failure;
    }
    return exit_ok;
}

bool toolchain_host_target(char *out, size_t out_size) {
    const char *argv[] = {pickup_program(), ARG_HOST, ARG_FORMAT, FORMAT_TOML, NULL};

    char answer[256] = "";
    if(process_capture(argv, answer, sizeof answer) != 0)
        return false;

    char parse_err[128] = "";
    toml_document *doc = toml_parse(answer, parse_err, sizeof parse_err);
    if(doc == NULL)
        return false;

    /* Top-level `target = "linux-x86_64"`, and nothing else: the answer is one
       key precisely so that a reader of it cannot drift. */
    const bool ok = toml_get_string(doc, "", "target", out, out_size) && out[0] != '\0';
    toml_free(doc);
    return ok;
}

int toolchain_resolve(const project_target *target, const char *platform, bool needs_cpp, wsdb *db,
                      bool refresh, resolved_toolchain *out) {
    /* An explicit choice by the user outranks resolution, and is not cached:
       the compile command already carries the path, so changing the variable
       rebuilds on its own. */
    if(take_from_environment(out))
        return exit_ok;

    char features[REQUEST_SIZE];
    char request[REQUEST_SIZE];
    if(!compose_features(target, features, sizeof features) ||
       !compose_request(target, platform, needs_cpp, features, request, sizeof request)) {
        fprintf(stderr, "molto: [target] requirements are too long to resolve\n");
        return exit_build_failure;
    }

    if(!refresh && take_from_wsdb(db, request, out))
        return exit_ok;

    int code = ask_pickup(target, platform, needs_cpp, features, out);
    if(code == exit_ok)
        remember(db, request, out);
    return code;
}

/* --- running what was built --- */

/* Which variable the loader reads, and how this platform separates a list of
   directories inside it. */
#ifdef _WIN32
#define RUNTIME_PATH_VAR "PATH"
#define RUNTIME_PATH_SEPARATOR ";"
#elif defined(__APPLE__)
#define RUNTIME_PATH_VAR "DYLD_LIBRARY_PATH"
#define RUNTIME_PATH_SEPARATOR ":"
#else
#define RUNTIME_PATH_VAR "LD_LIBRARY_PATH"
#define RUNTIME_PATH_SEPARATOR ":"
#endif

const char *toolchain_runtime_path_var(void) { return RUNTIME_PATH_VAR; }

bool toolchain_runtime_path(const resolved_toolchain *chain, char *out, size_t out_size) {
    if(chain->runtime_dir_count == 0)
        return false;

    size_t used = 0;
    for(size_t i = 0; i < chain->runtime_dir_count; i++) {
        const int written = snprintf(out + used, out_size - used, "%s%s",
                                     i > 0 ? RUNTIME_PATH_SEPARATOR : "", chain->runtime_dirs[i]);
        if(written < 0 || (size_t)written >= out_size - used)
            return false;
        used += (size_t)written;
    }

    /* The toolchain's directories go first and what the variable already held
       follows: a program built here must find the runtime it was built against
       ahead of any other copy on the machine, and everything the user's
       environment already pointed at still resolves behind it. */
    const char *inherited = getenv(RUNTIME_PATH_VAR);
    if(inherited != NULL && inherited[0] != '\0') {
        const int written =
            snprintf(out + used, out_size - used, "%s%s", RUNTIME_PATH_SEPARATOR, inherited);
        if(written < 0 || (size_t)written >= out_size - used)
            return false;
    }
    return true;
}
