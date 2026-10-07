#include <molto/services/configure_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
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
                           char hex_out[65]) {
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

static bool configure_now(const char *name, const char *root, const recipe_build *build,
                          const char *cc, const char *target, char *err, size_t err_size) {
    char cc_shell[CONFIGURE_PATH_MAX];
    shell_path(cc, cc_shell, sizeof cc_shell);

    /* CC first, then what the recipe asked for, which may add to it but is
       never allowed to choose another compiler: what molto compiles with and
       what configure tested have to be the same program. */
    process_env_var env[RECIPE_BUILD_MAX_ENV + 1];
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

    /* Minutes on Windows, where every one of configure's hundreds of test
       programs is a process MSYS2 has to start: a loader on a terminal, one
       line anywhere else. Its output is captured, so the loader is the only
       writer while it runs. */
    loader *spinner = NULL;
    if(progress_is_interactive(stderr)) {
        char label[LOADER_LABEL_MAX];
        snprintf(label, sizeof label, "configuring %s with its own configure", name);
        spinner = loader_start(stderr, label);
    } else {
        fprintf(stderr, "molto: configuring %s with upstream's configure (once per compiler)\n",
                name);
    }
    bool ok = run_step(argv, root, env, env_count, name, "configure", err, err_size);
    for(size_t i = 0; ok && i < build->target_count; i++)
        ok = make_target(root, build->targets[i], env, env_count, name, err, err_size);
    loader_stop(spinner);
    return ok;
}

bool configure_dependency(const char *name, const char *root, const recipe_build *build,
                          const char *cc, const char *target, char *err, size_t err_size) {
    if(!recipe_build_configures(build))
        return true;

    char want[65];
    configure_fingerprint(build, cc, target, want);
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
        ok = configure_now(name, root, build, cc, target, err, err_size);
        if(ok && !fs_write_file(stamp, want))
            ok = set_error(err, err_size, "could not record that '%s' is configured", name);
    }
    fs_lock_release(&lock);
    return ok;
}
