#include <molto/build/library.h>

#include <molto/services/fs_service.h>
#include <molto/util/semver.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

/* Written into one of the three fields, refusing rather than truncating: a
   soname cut short names a library that will not be found. */
static bool put(char *out, const char *format, ...) __attribute__((format(printf, 2, 3)));

static bool put(char *out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out, LIBRARY_NAME_MAX, format, args);
    va_end(args);
    return written >= 0 && written < LIBRARY_NAME_MAX;
}

static bool too_long(char *err, size_t err_size, const char *package) {
    return fail(err, err_size, "the package name '%s' is too long to build a library from",
                package);
}

/* `libfoo.so.1.2.3`, with `libfoo.so.1` recorded inside it and `libfoo.so`
   pointing at it.
 *
 * The soname carries the major and nothing else, which is the whole of the
 * convention: a program linked against `libfoo.so.1` keeps running when 1.2.3
 * is replaced by 1.9.0, and stops when it is replaced by 2.0.0. That is what
 * semver already promises, said in the one place a loader reads. */
static bool shared_names(const char *package, const char *version, library_names *out, char *err,
                         size_t err_size) {
    semver parsed;
    if(!semver_parse(version, &parsed))
        return fail(err, err_size,
                    "a shared library needs a major version and '%s' is not a version molto can "
                    "read; [package].version must be semver to build one",
                    version);

    if(!put(out->file, "lib%s.so.%lu.%lu.%lu", package, parsed.major, parsed.minor, parsed.patch) ||
       !put(out->soname, "lib%s.so.%lu", package, parsed.major) ||
       !put(out->devlink, "lib%s.so", package))
        return too_long(err, err_size, package);
    return true;
}

bool library_names_of(artifact_kind kind, const char *package, const char *version,
                      library_names *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    if(package == NULL || package[0] == '\0')
        return fail(err, err_size, "a package with no name cannot be built");

    switch(kind) {
    case artifact_executable:
        /* No `lib` and no extension: an executable is called what the package
           is called, which is what a person types to run it. */
        return put(out->file, "%s", package) || too_long(err, err_size, package);
    case artifact_static:
        return put(out->file, "lib%s.a", package) || too_long(err, err_size, package);
    case artifact_shared:
        return shared_names(package, version, out, err, err_size);
    case artifact_source:
        return fail(err, err_size,
                    "artifact 'source' describes a package a registry serves as sources, not "
                    "something to build here");
    }
    return fail(err, err_size, "unknown artifact kind");
}

/* --- the tool that makes one --- */

/* `<the compiler's directory>/<name>`, when there is such a file. */
static bool beside(const char *compiler, const char *name, char *out, size_t out_size) {
    const char *slash = strrchr(compiler, '/');
    if(slash == NULL)
        return false;
    const int written = snprintf(out, out_size, "%.*s/%s", (int)(slash - compiler), compiler, name);
    return written > 0 && (size_t)written < out_size && fs_path_exists(out);
}

bool library_archiver(const char *compiler, char *out, size_t out_size) {
    const char *chosen = getenv(LIBRARY_ARCHIVER_ENV);
    if(chosen != NULL && chosen[0] != '\0') {
        const int written = snprintf(out, out_size, "%s", chosen);
        return written > 0 && (size_t)written < out_size;
    }
    if(compiler != NULL &&
       (beside(compiler, "ar", out, out_size) || beside(compiler, "llvm-ar", out, out_size)))
        return true;
    const int written = snprintf(out, out_size, "%s", "ar");
    return written > 0 && (size_t)written < out_size;
}
