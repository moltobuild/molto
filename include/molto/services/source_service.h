#ifndef MOLTO_SOURCE_SERVICE_H
#define MOLTO_SOURCE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/util/doc.h>

/*
 * The `[source]` table of a recipe, and the cache it is fetched into.
 *
 * A source recipe (RFC-0009) has no bytes in the registry: it names an
 * upstream and the machine that wants the dependency goes and gets it. This
 * service is that half — read the table, fetch what it names, verify it, and
 * leave it unpacked under a path derived from the coordinate.
 *
 * What it deliberately does not do: build anything, or run anything the recipe
 * chose. `[build]` names a build system and that is a separate step; nothing
 * here executes a string that came out of a TOML file.
 *
 * The cache lives at ~/.molto/cache/sources/<name>/<version>/<target>, or
 * under $MOLTO_CACHE when that is set. A coordinate is fetched once: a
 * completed fetch leaves a stamp file, and only a directory carrying one is
 * treated as usable. A directory without it is remains of an interrupted
 * fetch and is removed rather than read.
 */

#define SOURCE_URL_MAX 1024
#define SOURCE_DIGEST_MAX 80
#define SOURCE_REF_MAX 128
#define SOURCE_PREFIX_MAX 256
#define SOURCE_PATH_MAX 1024

/* Where a source comes from. Exactly one per recipe: two would leave the
   consumer to choose, and a dependency that resolves differently on different
   machines is not a dependency. */
typedef enum {
    source_origin_archive,
    source_origin_git,
    source_origin_path,
} source_origin;

/* How an archive is packed.
 *
 * Declared by the recipe rather than inferred from the URL, because an
 * extension is a naming convention and not a fact: a URL may serve a tarball
 * from a path ending in `/download`, or end in `.zip` and serve something
 * else. `source_compression_infer` is what an older recipe that says nothing
 * still gets, so the key could be added without a flag day.
 *
 * Only an `archive` has one. A git clone is a checkout and a path is a
 * directory; neither is packed, so declaring it there means nothing and is an
 * error rather than a field quietly ignored. */
typedef enum {
    source_compression_infer, /* absent: decide from the URL's extension */
    source_compression_zip,
    source_compression_tar,
    source_compression_tar_gz,
    source_compression_tar_bz2,
    source_compression_tar_xz,
    source_compression_tar_zst,
} source_compression;

typedef struct {
    source_origin origin;
    /* The archive URL, the repository URL, or the local directory. */
    char location[SOURCE_URL_MAX];
    /* Lowercase hex sha256. Required for an archive, empty otherwise: a URL
       promises a location and not content, while a commit id is itself a
       digest. */
    char sha256[SOURCE_DIGEST_MAX];
    /* The git tag or revision to check out. Empty for the other origins. */
    char reference[SOURCE_REF_MAX];
    /* Leading directory to drop when unpacking, e.g. the one every release
       tarball wraps itself in. */
    char strip_prefix[SOURCE_PREFIX_MAX];
    /* Archive origins only. */
    source_compression compression;
} source_spec;

/* The name a compression is spelled with in a recipe, or "" for `infer`. */
[[nodiscard]] const char *source_compression_name(source_compression compression);

/* Read the `[source]` table of a parsed recipe, whichever way it arrived — TOML
   from a file, or JSON from a registry's `metadata`. Returns false, with a
   reason in `err`, when the table is missing, names no origin or names two, or
   an archive carries no digest. */
[[nodiscard]] bool source_read(doc_view doc, source_spec *out, char *err, size_t err_size);

/* True when `spec` is internally consistent: an archive carries a digest, and
   a digest is a lowercase hex sha256.

   Public because a manifest's `[deps]` entry produces the same struct under the
   same rules (RFC-0008), and one rule stated in two places is one rule that
   gets changed in one of them. */
[[nodiscard]] bool source_spec_validate(const source_spec *spec, char *err, size_t err_size);

/* The root of the source cache: `$MOLTO_CACHE`, or ~/.molto/cache.

   Exported because it is a bound a document is validated against (RFC-0013),
   and a caller that composed it itself would miss the override and validate
   against a directory this build never uses. False when there is no HOME and no
   override, which is the one case where there is no cache to name. */
[[nodiscard]] bool source_cache_root(char *out, size_t size);

/* The directory the cache holds one coordinate under. Does not create it, and
   does not say whether anything is there.

   False if $HOME is unset (and $MOLTO_CACHE with it), if the path would not
   fit, or if any segment is empty or holds a '/' or a "..". That last check is
   not defensive tidiness: a coordinate reaches here from a recipe, and a recipe
   comes from a registry — a `target` of "../../.." would otherwise put a fetch
   wherever it liked. */
[[nodiscard]] bool source_cache_path(const char *name, const char *version, const char *target,
                                     char *out, size_t size);

/* The two things the cache holds for a coordinate, kept in separate trees. */
#define SOURCE_CACHE_SOURCES "sources"
#define SOURCE_CACHE_RELEASES "releases"

/* The same path, under `area` rather than under the sources.
 *
 * There are two areas because the second outlives the first. A walk over
 * metadata learns what a version depends on without fetching a byte of it
 * (RFC-0008), so what the registry said has to be storable for a coordinate
 * whose sources are not on disk — and it must not live inside the directory a
 * later fetch replaces wholesale.
 *
 * Same refusals as `source_cache_path`, and for the same reason. */
[[nodiscard]] bool source_cache_area_path(const char *area, const char *name, const char *version,
                                          const char *target, char *out, size_t size);

/* The version segment a source of its own caches under: what makes two fetches
   of the same dependency the same fetch.

     archive  the sha256, so two recipes naming the same bytes share one entry
     git      the commit id the reference resolves to (RFC-0008), which for a
              `rev` is the reference itself and otherwise costs one ls-remote
     path     none: a path dependency is never cached, because its bytes are
              whatever is on disk right now

   A dependency resolved through a registry does not need this — it has a
   version, and the version is the segment. False, with a reason in `err`, for
   a path origin or when the reference cannot be resolved. */
[[nodiscard]] bool source_cache_key(const source_spec *spec, char *out, size_t size, char *err,
                                    size_t err_size);

/* True when that coordinate is already fetched and complete. */
[[nodiscard]] bool source_is_cached(const char *name, const char *version, const char *target);

/* Fetch `spec` into the cache and write the usable directory into `out`.
   Succeeds without doing anything when the coordinate is already there.

   A `path` origin is not copied: the directory the recipe names is used where
   it is, which is what makes it the origin for developing a recipe. */
[[nodiscard]] bool source_fetch(const source_spec *spec, const char *name, const char *version,
                                const char *target, char *out, size_t out_size, char *err,
                                size_t err_size);

#endif /* MOLTO_SOURCE_SERVICE_H */
