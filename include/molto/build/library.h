#ifndef MOLTO_BUILD_LIBRARY_H
#define MOLTO_BUILD_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/project_ctx.h>

/*
 * What a built thing is called (RFC-0007).
 *
 * The names are a convention and not a preference: `libz.so.1.2.13` with a
 * soname of `libz.so.1` is what every linker, every packager and every
 * `pkg-config` on a Unix expects to find, and a shared library that departs
 * from it is one nothing else can consume. Molto is not the audience for these
 * names — the rest of the system is.
 *
 * All four live in one place because they are one fact spelled four ways: the
 * file, the name recorded inside it, the name a linker looks for at `-lfoo`,
 * and the option that puts the second one there. Composed separately they would
 * eventually disagree, and a soname that names a file which does not exist is a
 * library that installs and then cannot be loaded.
 *
 * And they are one fact *per platform*, which is why nothing here is decided
 * before a platform is known. Linux writes `libfoo.so.1.2.3` and records
 * `libfoo.so.1`; macOS writes `libfoo.1.2.3.dylib` and records
 * `libfoo.1.dylib` — the version moved to the middle of the name, so this is
 * not an extension to swap. The option differs too: `-Wl,-soname` is a GNU ld
 * spelling and Apple's ld64 answers `unknown options: -soname`.
 *
 * Every name is relative to the profile's build directory, which is the only
 * anchor an artifact path has (RFC-0013).
 */

/* Room for `lib` + a package name + `.so.` + three numbers. A package name is
   capped at 128 by the manifest, so this cannot be reached by a name that was
   accepted. */
#define LIBRARY_NAME_MAX 192

typedef struct {
    /* The real file: `calculator`, `libcalculator.a`, `libcalculator.so.0.1.0`. */
    char file[LIBRARY_NAME_MAX];
    /* Recorded inside a shared library and looked for at load time. Empty for
       the kinds that carry none, which is every kind but `shared`. */
    char soname[LIBRARY_NAME_MAX];
    /* The unversioned name a `-lcalculator` resolves through, and the one a
       build against this library uses. Empty except for `shared`. */
    char devlink[LIBRARY_NAME_MAX];
    /* The whole linker option that records `soname` inside the file, ready to
       push onto a command line: `-Wl,-soname,libfoo.so.1` on a GNU linker,
       `-Wl,-install_name,libfoo.1.dylib` on Apple's. Composed here because this
       is the only place that knows the platform and the name at once, which
       spares every caller from knowing either. Empty except for `shared`. */
    char name_option[LIBRARY_NAME_MAX + 24];
} library_names;

/*
 * The names for one artifact kind.
 *
 * `platform` is the target triple this build is for, or NULL for the machine
 * running. It is a parameter and not a compile-time test because the answer is
 * about the target and not about the host: a `--target aarch64-darwin` from a
 * Linux box must name a dylib, and an `#ifdef` would name a `.so`.
 *
 * False with a message in `err` when the kind cannot be named: `source`
 * describes a package for a registry to serve rather than something to build
 * here, and `shared` needs a major version, so a `version` that is not semver
 * is refused rather than guessed at. A guess would put the wrong number in a
 * soname, which is the one place a wrong number is an ABI promise.
 */
[[nodiscard]] bool library_names_of(artifact_kind kind, const char *package, const char *version,
                                    const char *platform, library_names *out, char *err,
                                    size_t err_size);

/*
 * The archiver that turns objects into a `.a`.
 *
 * Looked for beside the resolved compiler before anywhere else, because a
 * toolchain's own archiver is the one that understands its objects: an LLVM
 * `clang` wants `llvm-ar` the moment `-flto` is involved, and binutils `ar`
 * fails on those objects with a message about a malformed file.
 *
 * Falling back to a bare `ar` is a departure from the rule that molto does not
 * search a PATH, and it is a deliberate one. The alternative is failing on the
 * ordinary machine where binutils is installed exactly as every compiler
 * expects, and a compiler already reaches `as` and `ld` the same way. Set
 * `MOLTO_AR` to decide it yourself; that wins over everything.
 *
 * False only when the answer does not fit the buffer.
 */
#define LIBRARY_ARCHIVER_ENV "MOLTO_AR"

[[nodiscard]] bool library_archiver(const char *compiler, char *out, size_t out_size);

#endif /* MOLTO_BUILD_LIBRARY_H */
