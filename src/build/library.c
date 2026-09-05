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

/* The same, for the one field that is not a name: an option carries a name and
   its flag, so it does not fit the buffer the other three share. */
static bool put_option(char *out, const char *format, ...) __attribute__((format(printf, 2, 3)));

static bool put_option(char *out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out, LIBRARY_NAME_MAX + 24, format, args);
    va_end(args);
    return written >= 0 && written < LIBRARY_NAME_MAX + 24;
}

static bool too_long(char *err, size_t err_size, const char *package) {
    return fail(err, err_size, "the package name '%s' is too long to build a library from",
                package);
}

/*
 * Whether the shared libraries of this build are Mach-O.
 *
 * Read off the target triple, and off the host only when there is no target --
 * which is what makes a `--target aarch64-darwin` from a Linux box name a dylib
 * instead of the host's `.so`. `darwin` is the spelling the registry and pickup
 * already use; `apple` and `macos` are accepted because a person typing
 * `--target` will reach for one of those.
 */
static bool targets_darwin(const char *platform) {
    if(platform == NULL || platform[0] == '\0') {
#ifdef __APPLE__
        return true;
#else
        return false;
#endif
    }
    return strstr(platform, "darwin") != NULL || strstr(platform, "apple") != NULL ||
           strstr(platform, "macos") != NULL;
}

/* `libfoo.so.1.2.3`, with `libfoo.so.1` recorded inside it and `libfoo.so`
   pointing at it -- or the Mach-O spelling of the same three facts.
 *
 * The recorded name carries the major and nothing else, which is the whole of
 * the convention: a program linked against `libfoo.so.1` keeps running when
 * 1.2.3 is replaced by 1.9.0, and stops when it is replaced by 2.0.0. That is
 * what semver already promises, said in the one place a loader reads. macOS
 * makes the same promise with `libfoo.1.dylib`.
 *
 * Note where the version sits. Linux appends it after the extension and macOS
 * puts it before, so there is no suffix to substitute and no way to write one
 * of these names in terms of the other -- which is why this is a branch rather
 * than a spelling. The option that records the name differs too: `-soname` is
 * GNU ld's, and Apple's ld64 answers `unknown options: -soname`. */
static bool shared_names(const char *package, const char *version, const char *platform,
                         library_names *out, char *err, size_t err_size) {
    semver parsed;
    if(!semver_parse(version, &parsed))
        return fail(err, err_size,
                    "a shared library needs a major version and '%s' is not a version molto can "
                    "read; [package].version must be semver to build one",
                    version);

    if(targets_darwin(platform)) {
        if(!put(out->file, "lib%s.%lu.%lu.%lu.dylib", package, parsed.major, parsed.minor,
                parsed.patch) ||
           !put(out->soname, "lib%s.%lu.dylib", package, parsed.major) ||
           !put(out->devlink, "lib%s.dylib", package) ||
           /* A bare filename and not an `@rpath`: what molto records has to be
              findable after the library is copied somewhere, and a path here
              would write this build directory inside an artifact whose whole
              purpose is to be copied elsewhere. It is the rule
              `build_place_shared_links` already follows for the two links. */
           !put_option(out->name_option, "-Wl,-install_name,%s", out->soname))
            return too_long(err, err_size, package);
        return true;
    }

    if(!put(out->file, "lib%s.so.%lu.%lu.%lu", package, parsed.major, parsed.minor, parsed.patch) ||
       !put(out->soname, "lib%s.so.%lu", package, parsed.major) ||
       !put(out->devlink, "lib%s.so", package) ||
       !put_option(out->name_option, "-Wl,-soname,%s", out->soname))
        return too_long(err, err_size, package);
    return true;
}

bool library_names_of(artifact_kind kind, const char *package, const char *version,
                      const char *platform, library_names *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    if(package == NULL || package[0] == '\0')
        return fail(err, err_size, "a package with no name cannot be built");

    switch(kind) {
    case artifact_executable:
        /* No `lib` and no extension: an executable is called what the package
           is called, which is what a person types to run it.

           Deliberately not the platform's filename. These names reach the IR,
           where `artifact.path` describes what a project builds and has to
           read the same on every machine -- and putting `.exe` here put it
           there, which is a host's answer written into a portable document.
           The suffix goes on where the path on disk is composed, in
           `build_service`, because that is the only place that wants a
           filename rather than a name. */
        return put(out->file, "%s", package) || too_long(err, err_size, package);
    case artifact_static:
        return put(out->file, "lib%s.a", package) || too_long(err, err_size, package);
    case artifact_shared:
        return shared_names(package, version, platform, out, err, err_size);
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
