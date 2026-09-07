#ifndef MOLTO_PACK_SERVICE_H
#define MOLTO_PACK_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Turning a directory into the archive a coordinate is published as.
 *
 * Publishing took an archive somebody else had already made, which meant every
 * publish was two commands and a chance to get the second one wrong: the wrong
 * compressor for the target, a name the registry would not find, entries in
 * whatever order the filesystem happened to return them. None of those are
 * caught by anything, and a coordinate is immutable once spent.
 *
 * What this does *not* know is what is inside the directory. Which files a
 * toolchain does not need, which driver of it emits for the published target,
 * whether the thing can compile a program at all -- those are questions about
 * llvm-mingw or about conda, not about publishing, and the answers live where
 * that knowledge already is: pickup's `scripts/pack_*.sh`, which stage a
 * prefix and then verify it by compiling and running a program with it. This
 * takes a directory that is already what it should be and makes it an
 * artifact.
 */

/* Longest packing name, counting the terminator: `tar.zst` and `tar.gz`. */
#define PACK_FORMAT_MAX 16

/*
 * How a target's artifact is packed when the recipe does not say.
 *
 * zstd everywhere it can be: it decompresses several times faster and packs an
 * LLVM tree to well under half what gzip manages.
 *
 * gzip for a Windows target, and gzip only. Pickup starts tar through
 * CreateProcess, which searches the system directory before PATH, so
 * C:\Windows\System32\tar.exe is what runs there no matter what else is
 * installed. That is bsdtar with libarchive linked against zlib alone -- every
 * other codec it hands to an outside program, which on Windows is either not
 * there or deadlocks against it past a pipe buffer of compressed input.
 */
[[nodiscard]] const char *pack_default_format(const char *target);

/* Whether this is a packing molto can produce and pickup can open. */
[[nodiscard]] bool pack_format_is_known(const char *format);

/* What a packed coordinate is called: `<name>-<version>-<target>.<format>`,
   the name pickup looks for. False if it would not fit. */
[[nodiscard]] bool pack_archive_name(const char *name, const char *version, const char *target,
                                     const char *format, char *out, size_t size);

/*
 * Pack `directory` into `archive`, which is overwritten if it exists.
 *
 * The contents go in at the root rather than under a directory of their own,
 * because pickup extracts with `--strip-components=0` and expects `bin/` to be
 * the first thing it finds.
 *
 * Packed as reproducibly as the tar that runs allows: nobody's uid on the way
 * out, no date inside the compressed stream, and a fixed entry order where the
 * tar can sort. A published coordinate cannot be replaced, so the only check
 * anyone can make on an artifact later is to pack the same tree again and
 * compare the digest — which is a check only if the bytes repeat.
 *
 * The tar that runs is not always the one that was installed. On Windows molto
 * starts it through CreateProcess, whose search reaches the system directory
 * first, so the tar in the system directory wins: bsdtar, which cannot sort at
 * all and whose libarchive carries zlib and no other codec. Both branches are
 * built from what that implementation actually accepts, asked rather than
 * assumed, and the one property that cannot be had another way is reported by
 * pack_is_reproducible rather than quietly dropped.
 */
[[nodiscard]] bool pack_directory(const char *directory, const char *archive, const char *format,
                                  char *err, size_t err_size);

/* Whether the tar that will run can produce the same bytes from the same tree.
   False for the tar Windows ships, which has no way to sort its entries: the
   archive is correct and the digest is still the digest of what was packed,
   but packing again may not reproduce it. Said out loud so a publisher knows
   which of the two they have. */
[[nodiscard]] bool pack_is_reproducible(void);

#endif /* MOLTO_PACK_SERVICE_H */
