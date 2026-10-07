#include <molto/services/configure_service.h>

#include <molto/build/library.h>
#include <molto/services/compile_lines.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/tool_service.h>
#include <molto/util/loader.h>
#include <molto/util/progress.h>
#include <molto/util/sha256.h>
#include <molto/util/thread.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Beside the source it describes, so `molto clean` of a project never makes
   another one configure again, and a fresh unpack starts unconfigured. */
#define STAMP_FILE ".molto-configured"
#define LOCK_FILE ".molto-configure.lock"
/* What the configured build said it compiles (RFC-0025), kept while the stamp
   holds and removed whenever configure runs again. */
#define SOURCES_FILE ".molto-sources"
/* Room for everything `make -n` prints. FFmpeg's is around 3 MB. */
#define DRY_RUN_MAX ((size_t)64 * 1024 * 1024)

#define CONFIGURE_PATH_MAX 4096
/* Enough of configure's own output to show where it stopped. */
#define CONFIGURE_TAIL 16384

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

static void hash_field(sha256_state *state, const char *label, const char *value) {
    sha256_update(state, label, strlen(label));
    sha256_update(state, "=", 1);
    sha256_update(state, value, strlen(value));
    sha256_update(state, "\n", 1);
}

/* The digest, with the NASM `{nasm}` became: another assembler is another
   answer, as another compiler is. */
static void fingerprint_with(const recipe_build *build, const char *cc, const char *target,
                             const configure_view *view, const char *nasm, char hex_out[65]) {
    sha256_state state;
    sha256_init(&state);
    hash_field(&state, "system", recipe_build_system_name(build->system));
    /* Absent for a recipe that lists its own sources, so every stamp written
       before RFC-0025 still holds. */
    if(build->sources != recipe_sources_recipe) {
        hash_field(&state, "sources", build->sources == recipe_sources_make ? "make" : "cmake");
        for(size_t i = 0; i < build->goal_count; i++)
            hash_field(&state, "goal", build->goals[i]);
    }
    if(nasm != NULL && nasm[0] != '\0')
        hash_field(&state, "nasm", nasm);
    for(size_t i = 0; i < build->arg_count; i++)
        hash_field(&state, "arg", build->args[i]);
    for(size_t i = 0; i < build->env_count; i++)
        hash_field(&state, "env", build->env[i]);
    for(size_t i = 0; i < build->target_count; i++)
        hash_field(&state, "target", build->targets[i]);
    hash_field(&state, "cc", cc);
    hash_field(&state, "host", target != NULL ? target : "");
    /* What it could see of its dependencies is part of the answer: another
       OpenSSL is another set of questions answered differently. */
    if(view != NULL && view->includes != NULL) {
        for(size_t i = 0; i < str_list_count(view->includes); i++)
            hash_field(&state, "include", str_list_get(view->includes, i));
    }
    if(view != NULL && view->link_flags != NULL) {
        for(size_t i = 0; i < str_list_count(view->link_flags); i++)
            hash_field(&state, "link", str_list_get(view->link_flags, i));
    }
    /* Which libraries it saw built, and from what: their sources are under
       their own digest-named roots, so the paths name the bytes. */
    for(size_t i = 0; view != NULL && i < view->library_count; i++) {
        hash_field(&state, "library", view->libraries[i].library);
        for(size_t s = 0; s < str_list_count(view->libraries[i].sources); s++)
            hash_field(&state, "library-source", str_list_get(view->libraries[i].sources, s));
    }
    sha256_finish(&state, hex_out);
}

void configure_fingerprint(const recipe_build *build, const char *cc, const char *target,
                           const configure_view *view, char hex_out[65]) {
    fingerprint_with(build, cc, target, view, NULL, hex_out);
}

/* The compiler as a shell reads it: forward slashes, which MSYS2's sh takes on
   Windows and every POSIX shell takes everywhere. */
static void shell_path(const char *path, char *out, size_t size) {
    snprintf(out, size, "%s", path);
    for(char *c = out; *c != '\0'; c++) {
        if(*c == '\\')
            *c = '/';
    }
}

/* --- placeholders (RFC-0025) --- */

#define PLACEHOLDER_CC "{cc}"
#define PLACEHOLDER_NASM "{nasm}"

static bool build_mentions(const recipe_build *build, const char *placeholder) {
    for(size_t i = 0; i < build->arg_count; i++) {
        if(strstr(build->args[i], placeholder) != NULL)
            return true;
    }
    for(size_t i = 0; i < build->env_count; i++) {
        if(strstr(build->env[i], placeholder) != NULL)
            return true;
    }
    return false;
}

/* Whether what is being configured for is x86, where an `.asm` file is
   assembled rather than skipped. */
static bool targets_x86(const char *target) {
    if(target != NULL && target[0] != '\0')
        return strncmp(target, "x86_64", 6) == 0 || strncmp(target, "amd64", 5) == 0 ||
               strncmp(target, "i386", 4) == 0 || strncmp(target, "i686", 4) == 0;
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    return true;
#else
    return false;
#endif
}

/* What `{nasm}` becomes: the NASM molto resolved, as a shell reads it, or ""
   when the recipe never says it. A configuration for another architecture
   never runs it, so there a missing NASM is `nasm` and not an error. */
static bool resolve_nasm(const char *name, const recipe_build *build, const char *target, char *out,
                         size_t size, char *err, size_t err_size) {
    out[0] = '\0';
    if(!build_mentions(build, PLACEHOLDER_NASM))
        return true;
    resolved_tool nasm;
    char reason[512] = "";
    if(tool_resolve_build("nasm", &nasm, reason, sizeof reason)) {
        shell_path(nasm.path, out, size);
        return true;
    }
    if(targets_x86(target))
        return set_error(err, err_size, "dependency '%s' assembles x86 code with NASM, and %s",
                         name, reason);
    snprintf(out, size, "nasm");
    return true;
}

/* `text` with every placeholder replaced, into `out`. False when the result
   does not fit. */
static bool expand(const char *text, const char *cc, const char *nasm, char *out, size_t size) {
    size_t used = 0;
    for(const char *c = text; *c != '\0';) {
        const char *value = NULL;
        size_t skip = 0;
        if(strncmp(c, PLACEHOLDER_CC, strlen(PLACEHOLDER_CC)) == 0) {
            value = cc;
            skip = strlen(PLACEHOLDER_CC);
        } else if(strncmp(c, PLACEHOLDER_NASM, strlen(PLACEHOLDER_NASM)) == 0) {
            value = nasm;
            skip = strlen(PLACEHOLDER_NASM);
        }
        if(value != NULL) {
            const size_t length = strlen(value);
            if(used + length >= size)
                return false;
            memcpy(out + used, value, length);
            used += length;
            c += skip;
            continue;
        }
        if(used + 1 >= size)
            return false;
        out[used++] = *c++;
    }
    out[used] = '\0';
    return true;
}

/* `build` with its arguments and environment expanded, into `out`. */
static bool expand_build(const char *name, const recipe_build *build, const char *cc,
                         const char *nasm, recipe_build *out, char *err, size_t err_size) {
    *out = *build;
    for(size_t i = 0; i < build->arg_count; i++) {
        if(!expand(build->args[i], cc, nasm, out->args[i], sizeof out->args[i]))
            return set_error(err, err_size,
                             "dependency '%s': [build].args '%s' is too long once expanded", name,
                             build->args[i]);
    }
    for(size_t i = 0; i < build->env_count; i++) {
        if(!expand(build->env[i], cc, nasm, out->env[i], sizeof out->env[i]))
            return set_error(err, err_size,
                             "dependency '%s': [build].env '%s' is too long once expanded", name,
                             build->env[i]);
    }
    return true;
}

/* One step, its output kept rather than shown: configure prints hundreds of
   lines nobody reads unless it fails, and then only the last ones matter. */
static bool run_step(const char *const argv[], const char *root, const process_env_var *env,
                     size_t env_count, const char *name, const char *what, char *err,
                     size_t err_size) {
    char *output = malloc(CONFIGURE_TAIL);
    if(output == NULL)
        return set_error(err, err_size, "out of memory configuring '%s'", name);
    output[0] = '\0';
    process_spec spec = {
        .env = env,
        .env_count = env_count,
        .stdout_to = process_stream_capture,
        .stderr_to = process_stream_capture,
        .capture = output,
        .capture_size = CONFIGURE_TAIL,
        .cwd = root,
    };
    const int code = process_execute(argv, &spec);
    if(code == 0) {
        free(output);
        return true;
    }
    if(code == 127) {
        free(output);
        return set_error(err, err_size,
                         "dependency '%s' is configured by upstream's %s, which needs '%s' on the "
                         "PATH (on Windows, MSYS2's)",
                         name, what, argv[0]);
    }
    /* The end of what it said, where configure names the test that failed —
       in the message rather than printed here, so a loader still on the
       screen is gone before it appears. */
    const size_t length = strlen(output);
    const char *tail = length > 600 ? output + length - 600 : output;
    const bool configure = strcmp(what, "configure") == 0;
    set_error(err, err_size, "dependency '%s': upstream's %s failed with exit code %d%s%s%s\n%s",
              name, what, code, configure ? "; its log is " : "", configure ? root : "",
              configure ? "/config.log" : "", tail);
    free(output);
    return false;
}

/* `src/port/pg_config_paths.h` is `make -C src/port pg_config_paths.h`. */
static bool make_target(const char *root, const char *target, const process_env_var *env,
                        size_t env_count, const char *name, char *err, size_t err_size) {
    char dir[RECIPE_BUILD_ARG_MAX] = ".";
    const char *file = target;
    const char *slash = strrchr(target, '/');
    if(slash != NULL) {
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - target), target);
        file = slash + 1;
    }
    const char *argv[] = {"make", "-C", dir, file, NULL};
    return run_step(argv, root, env, env_count, name, "make", err, err_size);
}

/* Room for a joined list of directories handed to a configuration. */
#define VIEW_TEXT_MAX 16384

/* `prefix` and `separator` around each entry of `list` that `wanted` keeps,
   into `out`: "-I/a -I/b", or "/a;/b". Entries are written as a shell would
   read them, with forward slashes. */
static void join_view(const str_list *list, const char *prefix, const char *separator,
                      bool (*wanted)(const char *entry, char *out, size_t size), char *out,
                      size_t size) {
    out[0] = '\0';
    size_t used = 0;
    for(size_t i = 0; list != NULL && i < str_list_count(list); i++) {
        char entry[CONFIGURE_PATH_MAX];
        if(!wanted(str_list_get(list, i), entry, sizeof entry))
            continue;
        char shell[CONFIGURE_PATH_MAX];
        shell_path(entry, shell, sizeof shell);
        const int wrote =
            snprintf(out + used, size - used, "%s%s%s", used == 0 ? "" : separator, prefix, shell);
        if(wrote < 0 || (size_t)wrote >= size - used)
            break;
        used += (size_t)wrote;
    }
}

static bool as_is(const char *entry, char *out, size_t size) {
    snprintf(out, size, "%s", entry);
    return true;
}

/* A `-L<dir>` link flag, as its directory. */
static bool library_dir(const char *entry, char *out, size_t size) {
    if(strncmp(entry, "-L", 2) != 0 || entry[2] == '\0')
        return false;
    snprintf(out, size, "%s", entry + 2);
    return true;
}

/* An include directory's parent when it is called include: the prefix CMake's
   find modules search under (<prefix>/include, <prefix>/lib). */
static bool prefix_of(const char *entry, char *out, size_t size) {
    snprintf(out, size, "%s", entry);
    char *slash = strrchr(out, '/');
    if(slash != NULL && strcmp(slash, "/include") == 0) {
        *slash = '\0';
        return true;
    }
    return true;
}

/* One step's loader on a terminal, or its one line anywhere else. */
static loader *announce(const char *name, const char *how) {
    if(progress_is_interactive(stderr)) {
        char label[LOADER_LABEL_MAX];
        snprintf(label, sizeof label, "configuring %.60s with %s", name, how);
        return loader_start(stderr, label);
    }
    fprintf(stderr, "molto: configuring %s with %s (once per compiler)\n", name, how);
    return NULL;
}

/* --- libraries a configuration sees built (RFC-0025) --- */

#define LIBRARIES_DIR ".molto-libs"

/* One library: each source compiled as its recipe compiles it, then archived.
   Done once per configuration, beside it; the build proper compiles the same
   sources again into molto's cache, as for any dependency. */
static bool build_library(const char *name, const char *root, const char *cc,
                          const configure_library *library, char *err, size_t err_size) {
    char dir[CONFIGURE_PATH_MAX];
    char archive[CONFIGURE_PATH_MAX];
    if(!fs_format_path(dir, sizeof dir, "%s/" LIBRARIES_DIR "/%s", root, library->library) ||
       !fs_format_path(archive, sizeof archive, "%s/" LIBRARIES_DIR "/lib%s.a", root,
                       library->library) ||
       !fs_make_dirs(dir))
        return set_error(err, err_size, "dependency '%s': no room to build lib%s.a", name,
                         library->library);
    char archiver[CONFIGURE_PATH_MAX];
    if(!library_archiver(cc, archiver, sizeof archiver))
        return set_error(err, err_size, "dependency '%s': no archiver for %s", name, cc);

    str_list archive_argv;
    str_list_init(&archive_argv);
    bool ok = str_list_push(&archive_argv, archiver) && str_list_push(&archive_argv, "rcs") &&
              str_list_push(&archive_argv, archive);
    for(size_t i = 0; ok && i < str_list_count(library->sources); i++) {
        char object[CONFIGURE_PATH_MAX];
        char std[RECIPE_STD_MAX + 8] = "";
        if(library->std != NULL && library->std[0] != '\0')
            snprintf(std, sizeof std, "-std=%s", library->std);
        if(!fs_format_path(object, sizeof object, "%s/%zu.o", dir, i)) {
            ok = set_error(err, err_size, "dependency '%s': no room to build lib%s.a", name,
                           library->library);
            break;
        }
        str_list argv;
        str_list_init(&argv);
        ok = str_list_push(&argv, cc) && str_list_push(&argv, "-c") &&
             str_list_push(&argv, str_list_get(library->sources, i)) &&
             str_list_push(&argv, "-o") && str_list_push(&argv, object);
        if(ok && std[0] != '\0')
            ok = str_list_push(&argv, std);
        for(size_t d = 0; ok && d < str_list_count(library->defines); d++) {
            char define[CONFIGURE_PATH_MAX];
            snprintf(define, sizeof define, "-D%s", str_list_get(library->defines, d));
            ok = str_list_push(&argv, define);
        }
        for(size_t d = 0; ok && d < str_list_count(library->includes); d++) {
            char include[CONFIGURE_PATH_MAX];
            snprintf(include, sizeof include, "-I%s", str_list_get(library->includes, d));
            ok = str_list_push(&argv, include);
        }
        for(size_t d = 0; ok && d < str_list_count(library->flags); d++)
            ok = str_list_push(&argv, str_list_get(library->flags, d));
        const char **list = ok ? process_argv_from_list(&argv) : NULL;
        if(list == NULL) {
            str_list_free(&argv);
            ok = set_error(err, err_size, "out of memory building lib%s.a", library->library);
            break;
        }
        ok = run_step(list, root, NULL, 0, name, "the compiler", err, err_size) &&
             str_list_push(&archive_argv, object);
        free((void *)list);
        str_list_free(&argv);
    }
    if(ok) {
        (void)remove(archive);
        const char **list = process_argv_from_list(&archive_argv);
        ok = list != NULL && run_step(list, root, NULL, 0, name, "the archiver", err, err_size);
        free((void *)list);
    }
    str_list_free(&archive_argv);
    return ok;
}

/* Every library `view` names, built, and the `-L` that finds them. */
static bool build_libraries(const char *name, const char *root, const char *cc,
                            const configure_view *view, char *link_dir, size_t link_dir_size,
                            char *err, size_t err_size) {
    link_dir[0] = '\0';
    if(view == NULL || view->library_count == 0)
        return true;
    for(size_t i = 0; i < view->library_count; i++) {
        if(!build_library(name, root, cc, &view->libraries[i], err, err_size))
            return false;
    }
    char dir[CONFIGURE_PATH_MAX];
    if(!fs_format_path(dir, sizeof dir, "%s/" LIBRARIES_DIR, root))
        return set_error(err, err_size, "the source of '%s' is too deep to configure", name);
    shell_path(dir, link_dir, link_dir_size);
    return true;
}

/* `sh ./configure <args>` in the source, then make for each target. */
static bool configure_autotools(const char *name, const char *root, const recipe_build *build,
                                const char *target, const configure_view *view,
                                const char *link_dir, process_env_var *env, size_t env_count,
                                char *err, size_t err_size) {
    /* What molto resolved, visible to configure's probes as any installed
       library would be. */
    static char cppflags[VIEW_TEXT_MAX];
    static char ldflags[VIEW_TEXT_MAX];
    join_view(view != NULL ? view->includes : NULL, "-I", " ", as_is, cppflags, sizeof cppflags);
    join_view(view != NULL ? view->link_flags : NULL, "-L", " ", library_dir, ldflags,
              sizeof ldflags);
    if(link_dir[0] != '\0') {
        const size_t used = strlen(ldflags);
        snprintf(ldflags + used, sizeof ldflags - used, "%s-L%s", used == 0 ? "" : " ", link_dir);
    }
    if(cppflags[0] != '\0')
        env[env_count++] = (process_env_var){.name = "CPPFLAGS", .value = cppflags};
    if(ldflags[0] != '\0')
        env[env_count++] = (process_env_var){.name = "LDFLAGS", .value = ldflags};

    char host[RECIPE_BUILD_ARG_MAX + 8] = "";
    const char *argv[RECIPE_BUILD_MAX_ARGS + 4];
    size_t argc = 0;
    argv[argc++] = "sh";
    argv[argc++] = "./configure";
    for(size_t i = 0; i < build->arg_count; i++)
        argv[argc++] = build->args[i];
    if(target != NULL && target[0] != '\0') {
        snprintf(host, sizeof host, "--host=%s", target);
        argv[argc++] = host;
    }
    argv[argc] = NULL;

    loader *spinner = announce(name, "upstream's configure");
    bool ok = run_step(argv, root, env, env_count, name, "configure", err, err_size);
    for(size_t i = 0; ok && i < build->target_count; i++)
        ok = make_target(root, build->targets[i], env, env_count, name, err, err_size);
    loader_stop(spinner);
    return ok;
}

/* `cmake -S . -B .molto-cmake -G Ninja` with the compiler molto resolved and
   the recipe's arguments, then `cmake --build` for each target. Nothing else
   is built: molto compiles [artifacts] against what the configuration wrote. */
static bool configure_cmake(const char *name, const char *root, const recipe_build *build,
                            const char *cc, const configure_view *view, const char *link_dir,
                            process_env_var *env, size_t env_count, char *err, size_t err_size) {
    resolved_tool cmake;
    resolved_tool ninja;
    char reason[512] = "";
    if(!tool_resolve_build("cmake", &cmake, reason, sizeof reason) ||
       !tool_resolve_build("ninja", &ninja, reason, sizeof reason))
        return set_error(err, err_size, "dependency '%s' is configured with CMake, and %s", name,
                         reason);

    static char prefixes[VIEW_TEXT_MAX];
    static char includes[VIEW_TEXT_MAX];
    static char libraries[VIEW_TEXT_MAX];
    static char cpath[VIEW_TEXT_MAX];
    join_view(view != NULL ? view->includes : NULL, "", ";", prefix_of, prefixes, sizeof prefixes);
    join_view(view != NULL ? view->includes : NULL, "", ";", as_is, includes, sizeof includes);
    join_view(view != NULL ? view->link_flags : NULL, "", ";", library_dir, libraries,
              sizeof libraries);
    if(link_dir[0] != '\0') {
        const size_t used = strlen(libraries);
        snprintf(libraries + used, sizeof libraries - used, "%s%s", used == 0 ? "" : ";", link_dir);
    }
    /* Every include directory, to every compile a check makes. A find module
       hands its checks one directory (OPENSSL_INCLUDE_DIR) and a package may
       need two: Debian keeps opensslconf.h under usr/include/<multiarch>,
       which gcc searches only in the system's own /usr/include. CPATH is read
       by gcc and clang alike, and a recipe's CMAKE_C_FLAGS stays its own. */
#ifdef _WIN32
    join_view(view != NULL ? view->includes : NULL, "", ";", as_is, cpath, sizeof cpath);
#else
    join_view(view != NULL ? view->includes : NULL, "", ":", as_is, cpath, sizeof cpath);
#endif
    if(cpath[0] != '\0')
        env[env_count++] = (process_env_var){.name = "CPATH", .value = cpath};

    char cc_shell[CONFIGURE_PATH_MAX];
    char ninja_shell[CONFIGURE_PATH_MAX];
    shell_path(cc, cc_shell, sizeof cc_shell);
    shell_path(ninja.path, ninja_shell, sizeof ninja_shell);
    static char d_cc[CONFIGURE_PATH_MAX + 32];
    static char d_ninja[CONFIGURE_PATH_MAX + 32];
    static char d_prefix[VIEW_TEXT_MAX + 32];
    static char d_include[VIEW_TEXT_MAX + 32];
    static char d_library[VIEW_TEXT_MAX + 32];
    snprintf(d_cc, sizeof d_cc, "-DCMAKE_C_COMPILER=%s", cc_shell);
    snprintf(d_ninja, sizeof d_ninja, "-DCMAKE_MAKE_PROGRAM=%s", ninja_shell);
    snprintf(d_prefix, sizeof d_prefix, "-DCMAKE_PREFIX_PATH=%s", prefixes);
    snprintf(d_include, sizeof d_include, "-DCMAKE_INCLUDE_PATH=%s", includes);
    snprintf(d_library, sizeof d_library, "-DCMAKE_LIBRARY_PATH=%s", libraries);

    const char *argv[RECIPE_BUILD_MAX_ARGS + 16];
    size_t argc = 0;
    argv[argc++] = cmake.path;
    argv[argc++] = "-S";
    argv[argc++] = ".";
    argv[argc++] = "-B";
    argv[argc++] = CONFIGURE_CMAKE_DIR;
    argv[argc++] = "-G";
    argv[argc++] = "Ninja";
    argv[argc++] = d_cc;
    argv[argc++] = d_ninja;
    argv[argc++] = "-DCMAKE_BUILD_TYPE=Release";
    /* What molto reads the list of sources from, when the recipe asks it to. */
    if(build->sources == recipe_sources_cmake)
        argv[argc++] = "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON";
    if(prefixes[0] != '\0')
        argv[argc++] = d_prefix;
    if(includes[0] != '\0')
        argv[argc++] = d_include;
    if(libraries[0] != '\0')
        argv[argc++] = d_library;
    for(size_t i = 0; i < build->arg_count; i++)
        argv[argc++] = build->args[i];
    argv[argc] = NULL;

    loader *spinner = announce(name, "its CMake");
    bool ok = run_step(argv, root, env, env_count, name, "cmake", err, err_size);
    for(size_t i = 0; ok && i < build->target_count; i++) {
        const char *step[] = {cmake.path, "--build",         CONFIGURE_CMAKE_DIR,
                              "--target", build->targets[i], NULL};
        ok = run_step(step, root, env, env_count, name, "cmake --build", err, err_size);
    }
    loader_stop(spinner);
    return ok;
}

static bool configure_now(const char *name, const char *root, const recipe_build *recipe,
                          const char *cc, const char *target, const configure_view *view,
                          const char *nasm, char *err, size_t err_size) {
    char cc_shell[CONFIGURE_PATH_MAX];
    shell_path(cc, cc_shell, sizeof cc_shell);
    /* What the recipe said, with `{cc}` and `{nasm}` made into what molto
       resolved: FFmpeg's configure takes its compiler as `--cc=` and never
       reads CC. Static, because it is large and a configuration runs one at a
       time under its lock. */
    static recipe_build expanded;
    if(!expand_build(name, recipe, cc_shell, nasm, &expanded, err, err_size))
        return false;
    const recipe_build *build = &expanded;

    /* CC first, then what the recipe asked for, which may add to it but is
       never allowed to choose another compiler: what molto compiles with and
       what configure tested have to be the same program. Room for the two
       variables autotools adds. */
    process_env_var env[RECIPE_BUILD_MAX_ENV + 3];
    char names[RECIPE_BUILD_MAX_ENV][RECIPE_BUILD_ARG_MAX];
    size_t env_count = 0;
    env[env_count++] = (process_env_var){.name = "CC", .value = cc_shell};
    for(size_t i = 0; i < build->env_count; i++) {
        const char *equals = strchr(build->env[i], '=');
        if(equals == NULL)
            continue;
        snprintf(names[i], sizeof names[i], "%.*s", (int)(equals - build->env[i]), build->env[i]);
        if(strcmp(names[i], "CC") == 0)
            return set_error(err, err_size,
                             "dependency '%s': [build].env may not set CC; molto passes the "
                             "compiler it resolved",
                             name);
        env[env_count++] = (process_env_var){.name = names[i], .value = equals + 1};
    }

    /* What it is to see built, built first. */
    char link_dir[CONFIGURE_PATH_MAX];
    if(!build_libraries(name, root, cc, view, link_dir, sizeof link_dir, err, err_size))
        return false;

    if(build->system == recipe_build_cmake)
        return configure_cmake(name, root, build, cc, view, link_dir, env, env_count, err,
                               err_size);
    return configure_autotools(name, root, build, target, view, link_dir, env, env_count, err,
                               err_size);
}

bool configure_dependency(const char *name, const char *root, const recipe_build *build,
                          const char *cc, const char *target, const configure_view *view, char *err,
                          size_t err_size) {
    if(!recipe_build_configures(build))
        return true;

    char nasm[CONFIGURE_PATH_MAX];
    if(!resolve_nasm(name, build, target, nasm, sizeof nasm, err, err_size))
        return false;
    char want[65];
    fingerprint_with(build, cc, target, view, nasm, want);
    char stamp[CONFIGURE_PATH_MAX];
    char lock_path[CONFIGURE_PATH_MAX];
    char sources[CONFIGURE_PATH_MAX];
    if(!fs_format_path(stamp, sizeof stamp, "%s/" STAMP_FILE, root) ||
       !fs_format_path(lock_path, sizeof lock_path, "%s/" LOCK_FILE, root) ||
       !fs_format_path(sources, sizeof sources, "%s/" SOURCES_FILE, root))
        return set_error(err, err_size, "the source of '%s' is too deep to configure", name);

    /* Two builds sharing the cache may reach the same source at once; the
       second waits and then finds the stamp the first wrote. */
    fs_lock lock;
    if(!fs_lock_take(lock_path, &lock))
        return set_error(err, err_size, "could not take the configure lock for '%s'", name);

    bool ok = true;
    char *have = fs_read_file(stamp);
    const bool configured = have != NULL && strncmp(have, want, 64) == 0;
    free(have);
    if(!configured) {
        /* The list belonged to the configuration being replaced. */
        remove(sources);
        ok = configure_now(name, root, build, cc, target, view, nasm, err, err_size);
        if(ok && !fs_write_file(stamp, want))
            ok = set_error(err, err_size, "could not record that '%s' is configured", name);
    }
    fs_lock_release(&lock);
    return ok;
}

/* --- what the configured build compiles (RFC-0025) --- */

/* `make -n -B V=1 <goals>`: every line make would run, whatever the tree's
   state, with the silent rules automake and FFmpeg use turned off. */
static bool ask_make(const char *name, const char *root, const recipe_build *build,
                     const compile_drivers *drivers, compile_lines *out, char *err,
                     size_t err_size) {
    const char *argv[RECIPE_BUILD_MAX_GOALS + 6];
    size_t argc = 0;
    argv[argc++] = "make";
    argv[argc++] = "-n";
    argv[argc++] = "-B";
    argv[argc++] = "V=1";
    for(size_t i = 0; i < build->goal_count; i++)
        argv[argc++] = build->goals[i];
    argv[argc] = NULL;

    char *output = malloc(DRY_RUN_MAX);
    if(output == NULL)
        return set_error(err, err_size, "out of memory asking '%s' what it compiles", name);
    output[0] = '\0';
    process_spec spec = {
        .stdout_to = process_stream_capture,
        .stderr_to = process_stream_capture,
        .capture = output,
        .capture_size = DRY_RUN_MAX,
        .cwd = root,
    };
    const int code = process_execute(argv, &spec);
    bool ok = true;
    if(code != 0) {
        const size_t length = strlen(output);
        ok = set_error(err, err_size,
                       "dependency '%s': `make -n` failed with exit code %d, so molto cannot "
                       "tell what it compiles\n%s",
                       name, code, length > 600 ? output + length - 600 : output);
    } else if(spec.truncated) {
        ok = set_error(err, err_size, "dependency '%s': `make -n` printed more than %zu bytes",
                       name, DRY_RUN_MAX);
    } else {
        ok = compile_lines_from_make(output, root, drivers, out, err, err_size);
    }
    free(output);
    return ok;
}

static bool ask_cmake(const char *name, const char *root, const compile_drivers *drivers,
                      compile_lines *out, char *err, size_t err_size) {
    char path[CONFIGURE_PATH_MAX];
    if(!fs_format_path(path, sizeof path, "%s/" CONFIGURE_CMAKE_DIR "/compile_commands.json", root))
        return set_error(err, err_size, "the source of '%s' is too deep to read", name);
    char *json = fs_read_file(path);
    if(json == NULL)
        return set_error(err, err_size, "dependency '%s': its CMake configuration wrote no %s",
                         name, path);
    const bool ok = compile_lines_from_database(json, root, drivers, out, err, err_size);
    free(json);
    return ok;
}

/* Everything the build generates on the way to its objects — FFmpeg's
   version header, the NEON tables a program it compiles first writes
   (`ops_neon.gen.S`), the macros its x86 assembly includes — made by upstream's
   own rules: make runs for the goals with every compiler and archiver it
   would call replaced by `true`, so each generator runs and nothing compiles.
   The programs it builds for itself use HOSTCC, which stays real. */
static bool make_generated(const char *name, const char *root, const recipe_build *build,
                           const compile_lines *lines, char *err, size_t err_size) {
    static const char *const SILENCED[] = {"CC=true", "CXX=true",    "OBJCC=true",
                                           "AS=true", "CCAS=true",   "X86ASM=true",
                                           "AR=true", "RANLIB=true", "STRIP=true"};
    const size_t silenced = sizeof SILENCED / sizeof SILENCED[0];
    char jobs[32];
    snprintf(jobs, sizeof jobs, "-j%zu", thread_cpu_count());
    const char *argv[RECIPE_BUILD_MAX_GOALS + sizeof SILENCED / sizeof SILENCED[0] + 4];
    size_t argc = 0;
    argv[argc++] = "make";
    argv[argc++] = jobs;
    for(size_t i = 0; i < silenced; i++)
        argv[argc++] = SILENCED[i];
    for(size_t i = 0; i < build->goal_count; i++)
        argv[argc++] = build->goals[i];
    argv[argc] = NULL;
    if(!run_step(argv, root, NULL, 0, name, "make", err, err_size))
        return false;

    for(size_t i = 0; i < lines->count; i++) {
        char path[CONFIGURE_PATH_MAX];
        if(!fs_format_path(path, sizeof path, "%s/%s", root, lines->lines[i].source))
            return set_error(err, err_size, "the source of '%s' is too deep to read", name);
        if(!fs_path_exists(path))
            return set_error(err, err_size,
                             "dependency '%s': upstream's build compiles '%s', which its make "
                             "did not write",
                             name, lines->lines[i].source);
    }
    return true;
}

bool configure_compile_lines(const char *name, const char *root, const recipe_build *build,
                             const char *cc, compile_lines *out, char *err, size_t err_size) {
    if(build->sources == recipe_sources_recipe)
        return true;
    char sources[CONFIGURE_PATH_MAX];
    char lock_path[CONFIGURE_PATH_MAX];
    if(!fs_format_path(sources, sizeof sources, "%s/" SOURCES_FILE, root) ||
       !fs_format_path(lock_path, sizeof lock_path, "%s/" LOCK_FILE, root))
        return set_error(err, err_size, "the source of '%s' is too deep to read", name);

    fs_lock lock;
    if(!fs_lock_take(lock_path, &lock))
        return set_error(err, err_size, "could not take the configure lock for '%s'", name);
    bool ok = true;
    if(!compile_lines_read(sources, out)) {
        /* The compiler as configure was given it, and NASM if there is one:
           the two programs whose lines are compile lines. */
        char cc_shell[CONFIGURE_PATH_MAX];
        shell_path(cc, cc_shell, sizeof cc_shell);
        char nasm_shell[CONFIGURE_PATH_MAX] = "";
        resolved_tool nasm;
        char ignored[512];
        if(tool_resolve_build("nasm", &nasm, ignored, sizeof ignored))
            shell_path(nasm.path, nasm_shell, sizeof nasm_shell);
        const compile_drivers drivers = {.cc = cc_shell,
                                         .nasm = nasm_shell[0] != '\0' ? nasm_shell : "nasm"};
        ok = build->sources == recipe_sources_make
                 ? ask_make(name, root, build, &drivers, out, err, err_size)
                 : ask_cmake(name, root, &drivers, out, err, err_size);
        if(ok && out->count == 0)
            ok = set_error(err, err_size,
                           "dependency '%s': upstream's build compiles nothing molto recognises",
                           name);
        if(ok && build->sources == recipe_sources_make)
            ok = make_generated(name, root, build, out, err, err_size);
        if(ok && !compile_lines_write(out, sources))
            ok = set_error(err, err_size, "could not record what '%s' compiles", name);
    }
    fs_lock_release(&lock);
    return ok;
}
