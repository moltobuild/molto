#include <molto/services/configure_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/tool_service.h>
#include <molto/util/loader.h>
#include <molto/util/progress.h>
#include <molto/util/sha256.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Beside the source it describes, so `molto clean` of a project never makes
   another one configure again, and a fresh unpack starts unconfigured. */
#define STAMP_FILE ".molto-configured"
#define LOCK_FILE ".molto-configure.lock"

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

void configure_fingerprint(const recipe_build *build, const char *cc, const char *target,
                           const configure_view *view, char hex_out[65]) {
    sha256_state state;
    sha256_init(&state);
    hash_field(&state, "system", recipe_build_system_name(build->system));
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
    sha256_finish(&state, hex_out);
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

/* `sh ./configure <args>` in the source, then make for each target. */
static bool configure_autotools(const char *name, const char *root, const recipe_build *build,
                                const char *target, const configure_view *view,
                                process_env_var *env, size_t env_count, char *err,
                                size_t err_size) {
    /* What molto resolved, visible to configure's probes as any installed
       library would be. */
    static char cppflags[VIEW_TEXT_MAX];
    static char ldflags[VIEW_TEXT_MAX];
    join_view(view != NULL ? view->includes : NULL, "-I", " ", as_is, cppflags, sizeof cppflags);
    join_view(view != NULL ? view->link_flags : NULL, "-L", " ", library_dir, ldflags,
              sizeof ldflags);
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
                            const char *cc, const configure_view *view, process_env_var *env,
                            size_t env_count, char *err, size_t err_size) {
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
    join_view(view != NULL ? view->includes : NULL, "", ";", prefix_of, prefixes, sizeof prefixes);
    join_view(view != NULL ? view->includes : NULL, "", ";", as_is, includes, sizeof includes);
    join_view(view != NULL ? view->link_flags : NULL, "", ";", library_dir, libraries,
              sizeof libraries);

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

static bool configure_now(const char *name, const char *root, const recipe_build *build,
                          const char *cc, const char *target, const configure_view *view, char *err,
                          size_t err_size) {
    char cc_shell[CONFIGURE_PATH_MAX];
    shell_path(cc, cc_shell, sizeof cc_shell);

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

    if(build->system == recipe_build_cmake)
        return configure_cmake(name, root, build, cc, view, env, env_count, err, err_size);
    return configure_autotools(name, root, build, target, view, env, env_count, err, err_size);
}

bool configure_dependency(const char *name, const char *root, const recipe_build *build,
                          const char *cc, const char *target, const configure_view *view, char *err,
                          size_t err_size) {
    if(!recipe_build_configures(build))
        return true;

    char want[65];
    configure_fingerprint(build, cc, target, view, want);
    char stamp[CONFIGURE_PATH_MAX];
    char lock_path[CONFIGURE_PATH_MAX];
    if(!fs_format_path(stamp, sizeof stamp, "%s/" STAMP_FILE, root) ||
       !fs_format_path(lock_path, sizeof lock_path, "%s/" LOCK_FILE, root))
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
        ok = configure_now(name, root, build, cc, target, view, err, err_size);
        if(ok && !fs_write_file(stamp, want))
            ok = set_error(err, err_size, "could not record that '%s' is configured", name);
    }
    fs_lock_release(&lock);
    return ok;
}
