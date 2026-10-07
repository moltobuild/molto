#include <molto/services/source_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/paths_service.h>
#include <molto/services/process_service.h>
#include <molto/services/recipe_service.h>
#include <molto/util/loader.h>
#include <molto/util/progress.h>
#include <molto/util/semver.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Written last, once everything else has landed. Its presence is the only
   thing that makes a cached directory usable: an unpack interrupted halfway
   leaves a directory that looks perfectly reasonable and is missing files. */
#define STAMP_FILE ".molto-fetched"

/* Suffix of the directory a fetch is assembled in before it is moved into
   place. The move is a rename, so a reader never sees a half-built tree. */
#define WORK_SUFFIX ".fetching"

#define SECTION "source"

static bool fail(char *err, size_t err_size, const char *message) {
    if(err != NULL && err_size > 0)
        snprintf(err, err_size, "%s", message);
    return false;
}

static bool fail_about(char *err, size_t err_size, const char *message, const char *subject) {
    if(err != NULL && err_size > 0)
        snprintf(err, err_size, "%s: %s", message, subject);
    return false;
}

/* The same, for the messages that have to name which entry went wrong. */
static bool fail_fmt(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail_fmt(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

/* --- reading the table --- */

/* The closed list of packings a recipe may declare. A name outside it is a
   rejected recipe rather than a fallback to guessing: a recipe that names a
   format molto cannot unpack should say so on the machine that reads it, not
   produce an empty directory on the machine that builds it. */
static const struct {
    const char *name;
    source_compression compression;
} COMPRESSIONS[] = {
    {"zip", source_compression_zip},       {"tar", source_compression_tar},
    {"tar.gz", source_compression_tar_gz}, {"tar.bz2", source_compression_tar_bz2},
    {"tar.xz", source_compression_tar_xz}, {"tar.zst", source_compression_tar_zst},
};

const char *source_compression_name(source_compression compression) {
    for(size_t i = 0; i < sizeof COMPRESSIONS / sizeof COMPRESSIONS[0]; i++) {
        if(COMPRESSIONS[i].compression == compression)
            return COMPRESSIONS[i].name;
    }
    return "";
}

/* The keys that open a [source] table, in the order they are looked for. */
static const struct {
    const char *key;
    source_origin origin;
} ORIGINS[] = {
    {"archive", source_origin_archive},
    {"git", source_origin_git},
    {"path", source_origin_path},
};

static void read_optional(doc_view doc, const char *key, char *out, size_t size) {
    if(!doc_get_string(doc, SECTION, key, out, size))
        out[0] = '\0';
}

/* Which origin the table declares. Two is an error rather than a precedence
   rule: a recipe that resolves one way here and another way elsewhere is a
   suggestion, not a dependency. */
static bool read_origin(doc_view doc, source_spec *out, char *err, size_t err_size) {
    bool found = false;
    for(size_t i = 0; i < sizeof ORIGINS / sizeof ORIGINS[0]; i++) {
        char value[SOURCE_URL_MAX];
        if(!doc_get_string(doc, SECTION, ORIGINS[i].key, value, sizeof value))
            continue;
        if(found)
            return fail(err, err_size, "[source] names more than one origin");
        found = true;
        out->origin = ORIGINS[i].origin;
        snprintf(out->location, sizeof out->location, "%s", value);
    }
    if(!found)
        return fail(err, err_size, "[source] names no origin: one of archive, git or path");
    return true;
}

/* A git reference is at most one of tag or rev; they name the same thing. */
static bool read_reference(doc_view doc, source_spec *out, char *err, size_t err_size) {
    char tag[SOURCE_REF_MAX];
    char rev[SOURCE_REF_MAX];
    const bool has_tag = doc_get_string(doc, SECTION, "tag", tag, sizeof tag);
    const bool has_rev = doc_get_string(doc, SECTION, "rev", rev, sizeof rev);

    if(has_tag && has_rev)
        return fail(err, err_size, "[source] sets both tag and rev, and they name the same thing");

    snprintf(out->reference, sizeof out->reference, "%s", has_tag ? tag : (has_rev ? rev : ""));
    return true;
}

static bool read_compression(doc_view doc, source_spec *out, char *err, size_t err_size) {
    char name[32];
    if(!doc_get_string(doc, SECTION, "compression_format", name, sizeof name)) {
        if(doc_has_key(doc, SECTION, "compression_format"))
            return fail(err, err_size, "[source] compression_format must be a string");
        out->compression = source_compression_infer;
        return true;
    }

    /* A clone and a directory are not packed, so there is nothing here to
       describe. Refused rather than ignored: a recipe that sets it beside a
       git URL believes something about what molto will do. */
    if(out->origin != source_origin_archive)
        return fail(err, err_size,
                    "[source] compression_format describes an archive, and this source is not one");

    for(size_t i = 0; i < sizeof COMPRESSIONS / sizeof COMPRESSIONS[0]; i++) {
        if(strcmp(COMPRESSIONS[i].name, name) == 0) {
            out->compression = COMPRESSIONS[i].compression;
            return true;
        }
    }
    char message[256];
    snprintf(message, sizeof message,
             "[source] compression_format '%s' is not one molto can unpack "
             "(zip, tar, tar.gz, tar.bz2, tar.xz, tar.zst)",
             name);
    return fail(err, err_size, message);
}

static bool is_hex_digest(const char *text) {
    size_t length = 0;
    for(const char *p = text; *p != '\0'; p++, length++) {
        const bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
        if(!hex)
            return false;
    }
    return length == 64;
}

bool source_spec_validate(const source_spec *spec, char *err, size_t err_size) {
    /* An archive without a digest is the failure this exists to prevent: the
       upstream re-rolls its tarball and every consumer silently compiles
       something else. A commit id is already a digest, which is why git needs
       none, and a local path is whatever is on disk right now, which is the
       point of it. */
    if(spec->origin == source_origin_archive && spec->sha256[0] == '\0')
        return fail(err, err_size, "an archive needs a sha256 to verify it against");
    if(spec->sha256[0] != '\0' && !is_hex_digest(spec->sha256))
        return fail(err, err_size, "sha256 is not a lowercase hex sha256 digest");
    return true;
}

bool source_read(doc_view doc, source_spec *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);

    if(!doc_has_table(doc, SECTION))
        return fail(err, err_size, "the recipe has no [source] table");
    if(!read_origin(doc, out, err, err_size))
        return false;
    if(!read_reference(doc, out, err, err_size))
        return false;

    read_optional(doc, "sha256", out->sha256, sizeof out->sha256);
    read_optional(doc, "strip_prefix", out->strip_prefix, sizeof out->strip_prefix);

    /* After the origin, because whether this key may appear at all depends on
       which origin was named. */
    if(!read_compression(doc, out, err, err_size))
        return false;

    return source_spec_validate(out, err, err_size);
}

/* --- the cache --- */

/* $MOLTO_CACHE overrides the default so a build can be pointed at a scratch
   directory without touching the user's own. */
static bool cache_root(char *out, size_t size) {
    const char *override = getenv("MOLTO_CACHE");
    if(override != NULL && override[0] != '\0') {
        /* A path from the environment, so in Molto's separator before anything
           is composed onto it (paths_service says why). */
        if(!fs_format_path(out, size, "%s", override))
            return false;
        fs_to_one_separator(out);
        return true;
    }

    return paths_molto_cache_dir(out, size);
}

bool source_cache_root(char *out, size_t size) { return cache_root(out, size); }

/* A coordinate segment that cannot escape the cache directory. The name and
   version of a registry dependency are checked by the registry, but the
   coordinate this path is built from arrives inside a recipe, and a recipe is
   something a remote party wrote. */
static bool is_safe_segment(const char *segment) {
    if(segment == NULL || segment[0] == '\0')
        return false;
    if(strcmp(segment, ".") == 0 || strcmp(segment, "..") == 0)
        return false;
    return strchr(segment, '/') == NULL;
}

bool source_cache_area_path(const char *area, const char *name, const char *version,
                            const char *target, char *out, size_t size) {
    if(!is_safe_segment(area) || !is_safe_segment(name) || !is_safe_segment(version) ||
       !is_safe_segment(target))
        return false;

    char root[SOURCE_PATH_MAX];
    if(!cache_root(root, sizeof root))
        return false;
    return fs_format_path(out, size, "%s/%s/%s/%s/%s", root, area, name, version, target);
}

bool source_cache_path(const char *name, const char *version, const char *target, char *out,
                       size_t size) {
    return source_cache_area_path(SOURCE_CACHE_SOURCES, name, version, target, out, size);
}

/* The commit id a reference names, asked of the remote rather than of a clone:
   the answer decides the cache directory, so it has to be known before there
   is anything to put in one. */
static bool resolve_git_reference(const source_spec *spec, char *out, size_t size, char *err,
                                  size_t err_size) {
    const char *argv[] = {"git", "ls-remote", spec->location, spec->reference, NULL};
    char captured[512] = "";
    const int code = process_capture(argv, captured, sizeof captured);

    if(code == 127)
        return fail(err, err_size,
                    "git is not installed, and molto needs it to resolve a branch "
                    "or a tag to a commit");
    if(code != 0)
        return fail_about(err, err_size, "could not reach the repository", spec->location);

    /* "<sha>\t<ref>", one line per match; the first is the one asked for. */
    const size_t length = strcspn(captured, " \t\r\n");
    if(length == 0 || length >= size)
        return fail_about(err, err_size, "the repository knows no such branch, tag or commit",
                          spec->reference);
    snprintf(out, size, "%.*s", (int)length, captured);
    return true;
}

/* A 40-character (or 64, for sha256 repositories) lowercase hex string is
   already a commit id, so asking the remote what it resolves to would be one
   network round trip to be told what was written. */
static bool looks_like_commit_id(const char *reference) {
    const size_t length = strlen(reference);
    if(length != 40 && length != 64)
        return false;
    for(const char *p = reference; *p != '\0'; p++) {
        const bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
        if(!hex)
            return false;
    }
    return true;
}

/* A remote's default branch, from the symbolic ref it advertises for HEAD:
   "ref: refs/heads/<branch>\tHEAD". */
static bool remote_default_branch(const char *url, char *out, size_t size, char *err,
                                  size_t err_size) {
    const char *argv[] = {"git", "ls-remote", "--symref", url, "HEAD", NULL};
    char captured[1024] = "";
    const int code = process_capture(argv, captured, sizeof captured);
    if(code == 127)
        return fail(err, err_size,
                    "git is not installed, and molto needs it to ask a "
                    "repository for its default branch");
    if(code != 0)
        return fail_about(err, err_size, "could not reach the repository", url);

    static const char prefix[] = "ref: refs/heads/";
    const char *start = strstr(captured, prefix);
    if(start == NULL)
        return fail_about(err, err_size, "the repository names no default branch; add #<ref>", url);
    start += sizeof prefix - 1;
    const size_t length = strcspn(start, " \t\r\n");
    if(length == 0 || length >= size)
        return fail_about(err, err_size, "the repository's default branch is unreadable", url);
    snprintf(out, size, "%.*s", (int)length, start);
    return true;
}

/* Whether `listing`, the output of ls-remote, has a line for `full_ref`. */
static bool lists_ref(const char *listing, const char *full_ref) {
    const size_t length = strlen(full_ref);
    for(const char *at = strstr(listing, full_ref); at != NULL; at = strstr(at + 1, full_ref)) {
        const bool starts = at > listing && at[-1] == '\t';
        const char end = at[length];
        if(starts && (end == '\0' || end == '\n' || end == '\r' || end == '^'))
            return true;
    }
    return false;
}

bool source_git_reference_key(const char *url, const char *reference, const char **key, char *out,
                              size_t size, char *err, size_t err_size) {
    /* Both reach git's command line, where a leading dash is an option. */
    if(url[0] == '-' || (reference != NULL && reference[0] == '-'))
        return fail(err, err_size, "a git URL or reference may not begin with '-'");

    if(reference == NULL || reference[0] == '\0') {
        *key = "branch";
        return remote_default_branch(url, out, size, err, err_size);
    }
    if(strlen(reference) >= size)
        return fail_about(err, err_size, "git reference too long", reference);
    snprintf(out, size, "%s", reference);
    if(looks_like_commit_id(reference)) {
        *key = "rev";
        return true;
    }

    char tag_ref[256];
    char branch_ref[256];
    snprintf(tag_ref, sizeof tag_ref, "refs/tags/%s", reference);
    snprintf(branch_ref, sizeof branch_ref, "refs/heads/%s", reference);
    const char *argv[] = {"git", "ls-remote", url, tag_ref, branch_ref, NULL};
    char captured[2048] = "";
    const int code = process_capture(argv, captured, sizeof captured);
    if(code == 127)
        return fail(err, err_size,
                    "git is not installed, and molto needs it to tell a branch "
                    "from a tag");
    if(code != 0)
        return fail_about(err, err_size, "could not reach the repository", url);

    /* A name that is both is the tag: it is the one that does not move. */
    if(lists_ref(captured, tag_ref)) {
        *key = "tag";
        return true;
    }
    if(lists_ref(captured, branch_ref)) {
        *key = "branch";
        return true;
    }
    return fail_about(err, err_size, "the repository knows no such branch or tag", reference);
}

bool source_cache_key(const source_spec *spec, char *out, size_t size, char *err, size_t err_size) {
    switch(spec->origin) {
    case source_origin_archive:
        if(spec->sha256[0] == '\0')
            return fail(err, err_size, "an archive caches under its sha256, and it has none");
        snprintf(out, size, "%s", spec->sha256);
        return true;

    case source_origin_git:
        if(spec->reference[0] == '\0')
            return fail(err, err_size,
                        "a git source caches under a commit, so it needs a branch, tag or rev");
        if(looks_like_commit_id(spec->reference)) {
            snprintf(out, size, "%s", spec->reference);
            return true;
        }
        return resolve_git_reference(spec, out, size, err, err_size);

    case source_origin_path:
        return fail(err, err_size, "a path dependency is used where it is and is never cached");
    }
    return false;
}

static bool stamp_path(const char *directory, char *out, size_t size) {
    return fs_format_path(out, size, "%s/%s", directory, STAMP_FILE);
}

static bool is_complete(const char *directory) {
    char stamp[SOURCE_PATH_MAX];
    return stamp_path(directory, stamp, sizeof stamp) && fs_path_exists(stamp);
}

bool source_is_cached(const char *name, const char *version, const char *target) {
    char directory[SOURCE_PATH_MAX];
    return source_cache_path(name, version, target, directory, sizeof directory) &&
           is_complete(directory);
}

/* --- running the tools --- */

/* Molto shells out here for the same reason it shells out to a compiler: curl,
   tar and git are present wherever a build is, and an HTTP client, an archive
   reader and a git implementation inside molto would be three more things to
   keep patched. */
static bool run(const char *const argv[], char *err, size_t err_size, const char *what) {
    const int code = process_run(argv);
    if(code == 127)
        return fail_about(err, err_size, "not installed, and molto needs it to fetch a source",
                          argv[0]);
    if(code != 0)
        return fail_about(err, err_size, "failed", what);
    return true;
}

static bool checksum_with(const char *const argv[], char *out, size_t size) {
    char captured[256] = "";
    if(process_capture(argv, captured, sizeof captured) != 0)
        return false;

    const size_t length = strcspn(captured, " \t\r\n");
    if(length != 64 || length >= size)
        return false;
    snprintf(out, size, "%.*s", (int)length, captured);
    return true;
}

/*
 * The digest, from whichever tool the machine has.
 *
 * The note above says curl, tar and git are present wherever a build is. This
 * is the one that is not: GNU coreutils calls it `sha256sum` — Linux, and MSYS2
 * on Windows — while macOS ships Perl's `shasum` and no `sha256sum` at all.
 *
 * What that cost while it stood: `verify` fails closed, so on a Mac every
 * archive dependency was refused for being unverifiable rather than fetched.
 * Not a slow path or a warning — the whole dependency path, on a platform molto
 * claims to support. The suite said so in six places and every one of them read
 * like a test problem.
 *
 * Both print the same shape, `<64 hex>` then the filename, so only the command
 * differs.
 */
static bool checksum_of(const char *file, char *out, size_t size) {
    const char *coreutils[] = {"sha256sum", "--binary", file, NULL};
    if(checksum_with(coreutils, out, size))
        return true;
    const char *perl[] = {"shasum", "-a", "256", "-b", file, NULL};
    return checksum_with(perl, out, size);
}

static bool verify(const char *file, const char *expected, char *err, size_t err_size) {
    char actual[SOURCE_DIGEST_MAX] = "";
    if(!checksum_of(file, actual, sizeof actual))
        return fail(err, err_size,
                    "could not hash the downloaded archive: neither sha256sum nor shasum "
                    "answered, and molto will not accept bytes it cannot check");
    if(strcmp(actual, expected) != 0) {
        if(err != NULL && err_size > 0)
            snprintf(err, err_size, "the archive hashes to %s, and the recipe expects %s", actual,
                     expected);
        return false;
    }
    return true;
}

/* --- fetching --- */

static bool ends_with(const char *text, const char *suffix) {
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length && strcmp(text + text_length - suffix_length, suffix) == 0;
}

/* The name to save a download under: the URL's last segment, without any
   query string.

   It matters because the extension is how the format is decided, and a file
   saved under a name of molto's own choosing has none: sqlite.org's .zip
   downloaded as "archive" was handed to tar, which reported that it did not
   look like a tar archive. Keeping upstream's name keeps the answer. */
static void download_name(const char *url, char *out, size_t size) {
    const char *last = strrchr(url, '/');
    const char *name = last == NULL ? url : last + 1;
    const size_t length = strcspn(name, "?#");

    if(length == 0 || length >= size)
        snprintf(out, size, "%s", "archive");
    else
        snprintf(out, size, "%.*s", (int)length, name);
}

/* What an older recipe, written before compression_format existed, still gets.
   A zip is the one thing tar does not read; everything else it detects for
   itself, which is why one fallback covers every other packing. */
static source_compression infer_compression(const char *archive) {
    return ends_with(archive, ".zip") ? source_compression_zip : source_compression_tar;
}

/*
 * xz on Windows.
 *
 * Windows' own tar is bsdtar built without liblzma: handed a .tar.xz it does
 * not fail, it hangs, extracting nothing — measured on a GitHub runner, where
 * a native program finds C:\Windows\System32\tar.exe before anything on the
 * PATH. So on Windows molto takes the xz off itself, with `xz` — $MOLTO_XZ, the
 * one on the PATH, or the tukaani project's own Windows build, downloaded once
 * and verified — and hands tar a plain tar. Everywhere else tar reads xz.
 */
#ifdef _WIN32
#define XZ_ENV "MOLTO_XZ"
#define XZ_TOOL_DIR "tools/xz-5.8.4"
#define XZ_WINDOWS_URL                                                                             \
    "https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4-windows.zip"
#define XZ_WINDOWS_SHA256 "f31af7638391ecf286d48bc8555ce6e131691a1c52a9e44303bce57069e9de56"

static const unsigned char XZ_MAGIC[] = {0xfd, '7', 'z', 'X', 'Z', 0x00};

static bool starts_with_xz(const char *path) {
    FILE *file = fopen(path, "rb");
    if(file == NULL)
        return false;
    unsigned char magic[sizeof XZ_MAGIC] = {0};
    const bool read = fread(magic, 1, sizeof magic, file) == sizeof magic;
    (void)fclose(file);
    return read && memcmp(magic, XZ_MAGIC, sizeof XZ_MAGIC) == 0;
}

static bool download(const char *url, const char *into, char *err, size_t err_size);
static bool verify(const char *file, const char *expected, char *err, size_t err_size);

static bool xz_answers(const char *program) {
    const char *argv[] = {program, "--version", NULL};
    char ignored[256] = "";
    return process_capture(argv, ignored, sizeof ignored) == 0;
}

static bool xz_program(char *out, size_t size, char *err, size_t err_size) {
    const char *chosen = getenv(XZ_ENV);
    if(chosen != NULL && chosen[0] != '\0') {
        snprintf(out, size, "%s", chosen);
        return true;
    }
    if(xz_answers("xz")) {
        snprintf(out, size, "xz");
        return true;
    }
    char cache[SOURCE_PATH_MAX];
    char directory[SOURCE_PATH_MAX];
    if(!cache_root(cache, sizeof cache) ||
       !fs_format_path(directory, sizeof directory, "%s/" XZ_TOOL_DIR, cache) ||
       !fs_format_path(out, size, "%s/xz.exe", directory))
        return fail(err, err_size, "there is no room in molto's cache for xz");
    if(fs_path_exists(out))
        return true;
    char archive[SOURCE_PATH_MAX];
    if(!fs_make_dirs(directory) ||
       !fs_format_path(archive, sizeof archive, "%s/xz-windows.zip", directory))
        return fail_about(err, err_size, "could not create", directory);
    if(!download(XZ_WINDOWS_URL, archive, err, err_size) ||
       !verify(archive, XZ_WINDOWS_SHA256, err, err_size))
        return false;
    /* The release keeps the 64-bit build under bin_x86-64/; tar reads a zip. */
    const char *argv[] = {"tar", "-xf", archive, "-C", directory, "--strip-components=1",
                          /* The whole directory, never a library by name: the
                             release job reads every DLL name the binary holds
                             as an import, and xz.exe needs nothing beside it
                             that the directory does not carry. */
                          "bin_x86-64", NULL};
    const bool ok = run(argv, err, err_size, "tar");
    (void)remove(archive);
    return (ok && fs_path_exists(out)) || fail(err, err_size, "the xz release holds no xz.exe");
}

/* `<archive>.unxz.tar.xz`, decompressed in place to `<archive>.unxz.tar`:
   xz writes beside its input and has no option to write anywhere else. */
static bool unpack_xz(const char *archive, const char *into, char *err, size_t err_size) {
    char xz[SOURCE_PATH_MAX];
    char packed[SOURCE_PATH_MAX];
    char plain[SOURCE_PATH_MAX];
    if(!xz_program(xz, sizeof xz, err, err_size))
        return false;
    if(!fs_format_path(packed, sizeof packed, "%s.unxz.tar.xz", archive) ||
       !fs_format_path(plain, sizeof plain, "%s.unxz.tar", archive))
        return fail(err, err_size, "the archive path is too long");
    (void)remove(plain);
    if(!fs_copy_file(archive, packed))
        return fail_about(err, err_size, "could not copy the archive to decompress it", archive);
    const char *decompress[] = {xz, "-d", "-f", "-q", packed, NULL};
    bool ok = run(decompress, err, err_size, "xz");
    if(ok) {
        const char *argv[] = {"tar", "-xf", plain, "-C", into, NULL};
        ok = run(argv, err, err_size, "tar");
    }
    (void)remove(packed);
    (void)remove(plain);
    return ok;
}
#endif

/* `-xf` lets tar detect gzip, bzip2, xz and zstd from the bytes rather than
   from the name, so every tar packing runs the same command. Naming them in
   the recipe is still worth it: an unpackable format is then a rejected recipe
   instead of a tar that fails halfway through a build. */
static bool unpack(const source_spec *spec, const char *archive, const char *into, char *err,
                   size_t err_size) {
    const source_compression compression = spec->compression == source_compression_infer
                                               ? infer_compression(archive)
                                               : spec->compression;

    if(compression == source_compression_zip) {
        const char *argv[] = {"unzip", "-q", archive, "-d", into, NULL};
        return run(argv, err, err_size, "unzip");
    }
#ifdef _WIN32
    if(starts_with_xz(archive))
        return unpack_xz(archive, into, err, err_size);
#endif
    const char *argv[] = {"tar", "-xf", archive, "-C", into, NULL};
    return run(argv, err, err_size, "tar");
}

static bool download(const char *url, const char *into, char *err, size_t err_size) {
    /* --fail so an error page is a failure and not a file: without it curl
       writes the 404 body to disk and reports success. */
    const char *argv[] = {"curl", "--fail", "--silent", "--show-error", "--location", "--output",
                          into,   url,      NULL};
    return run(argv, err, err_size, "curl");
}

static bool clone(const source_spec *spec, const char *into, char *err, size_t err_size) {
    const char *clone_argv[] = {"git", "clone", "--quiet", spec->location, into, NULL};
    if(!run(clone_argv, err, err_size, "git clone"))
        return false;
    if(spec->reference[0] == '\0')
        return true;

    const char *checkout_argv[] = {"git", "-C", into, "checkout", "--quiet", spec->reference, NULL};
    return run(checkout_argv, err, err_size, "git checkout");
}

/* Whatever the recipe says to unwrap, inside the directory just unpacked. */
static bool unpacked_root(const char *work, const char *strip_prefix, char *out, size_t size) {
    if(strip_prefix[0] == '\0')
        return fs_format_path(out, size, "%s", work);
    return fs_format_path(out, size, "%s/%s", work, strip_prefix);
}

static bool fetch_archive(const source_spec *spec, const char *work, char *err, size_t err_size) {
    char name[SOURCE_PREFIX_MAX];
    download_name(spec->location, name, sizeof name);

    char archive[SOURCE_PATH_MAX];
    if(!fs_format_path(archive, sizeof archive, "%s/%s", work, name))
        return fail(err, err_size, "the download path is too long");

    if(!download(spec->location, archive, err, err_size))
        return false;
    if(!verify(archive, spec->sha256, err, err_size))
        return false;
    if(!unpack(spec, archive, work, err, err_size))
        return false;

    (void)remove(archive);
    return true;
}

/* Everything a fetch needs to assemble before anything is moved into place. */
static bool assemble(const source_spec *spec, const char *work, char *root, size_t root_size,
                     char *err, size_t err_size) {
    if(!fs_make_dirs(work))
        return fail_about(err, err_size, "could not create the cache directory", work);

    if(spec->origin == source_origin_git) {
        if(!clone(spec, work, err, err_size))
            return false;
    } else if(!fetch_archive(spec, work, err, err_size)) {
        return false;
    }

    if(!unpacked_root(work, spec->strip_prefix, root, root_size))
        return fail(err, err_size, "the unpacked path is too long");
    if(!fs_is_dir(root))
        return fail_about(err, err_size, "the strip_prefix names no directory in the source",
                          spec->strip_prefix);
    return true;
}

/* The fetched tree replaces whatever was at `destination`, in one rename, so
   no reader ever sees a directory being filled. */
static bool install(const char *root, const char *destination, char *err, size_t err_size) {
    if(!fs_remove_tree(destination))
        return fail_about(err, err_size, "could not clear", destination);
    if(rename(root, destination) != 0)
        return fail_about(err, err_size, "could not move the fetched source into", destination);

    char stamp[SOURCE_PATH_MAX];
    if(!stamp_path(destination, stamp, sizeof stamp) || !fs_write_file(stamp, "ok\n"))
        return fail(err, err_size, "could not record that the fetch completed");
    return true;
}

/* A local directory is used where it is. Copying it would mean a recipe under
   development stopped tracking the edits being made to it, which is the one
   thing this origin exists for. */
static bool use_local_path(const source_spec *spec, char *out, size_t out_size, char *err,
                           size_t err_size) {
    if(!fs_is_dir(spec->location))
        return fail_about(err, err_size, "[source] path is not a directory", spec->location);
    snprintf(out, out_size, "%s", spec->location);
    return true;
}

/* Where the bytes come from, short enough for one line: the archive's file
   name, or the repository and the first characters of the commit. */
static void describe_origin(const source_spec *spec, char *out, size_t size) {
    if(spec->origin == source_origin_git) {
        snprintf(out, size, "%s at %.10s", spec->location, spec->reference);
        return;
    }
    const char *slash = strrchr(spec->location, '/');
    snprintf(out, size, "%s", slash != NULL && slash[1] != '\0' ? slash + 1 : spec->location);
}

/* A download can take minutes — PostgreSQL's tarball is six thousand files to
   unpack — so it says so. On a terminal that is a loader animating its name;
   elsewhere one line, the way a build without a terminal prints one line per
   source, so a CI log shows where the time went. Said only when bytes move: a
   source already in the cache is not news. */
static loader *announce_fetch(const char *name, const source_spec *spec) {
    /* Room for the longest location and " at " and ten characters of commit,
       so nothing is cut here; the label below decides what it shows. */
    char origin[SOURCE_URL_MAX + 16];
    describe_origin(spec, origin, sizeof origin);
    if(!progress_is_interactive(stderr)) {
        fprintf(stderr, "molto: fetching %s (%s)\n", name, origin);
        return NULL;
    }
    char label[LOADER_LABEL_MAX];
    snprintf(label, sizeof label, "fetching %.40s (%.70s)", name, origin);
    return loader_start(stderr, label);
}

bool source_fetch(const source_spec *spec, const char *name, const char *version,
                  const char *target, char *out, size_t out_size, char *err, size_t err_size) {
    if(spec->origin == source_origin_path)
        return use_local_path(spec, out, out_size, err, err_size);

    char destination[SOURCE_PATH_MAX];
    if(!source_cache_path(name, version, target, destination, sizeof destination))
        return fail(err, err_size,
                    "this machine has no home directory, so there is nowhere to cache a "
                    "source; set MOLTO_HOME or MOLTO_CACHE to a directory molto may use");

    if(is_complete(destination)) {
        snprintf(out, out_size, "%s", destination);
        return true;
    }

    char work[SOURCE_PATH_MAX];
    if(!fs_format_path(work, sizeof work, "%s%s", destination, WORK_SUFFIX))
        return fail(err, err_size, "the cache path is too long");
    if(!fs_remove_tree(work))
        return fail_about(err, err_size, "could not clear", work);

    char root[SOURCE_PATH_MAX];
    loader *spinner = announce_fetch(name, spec);
    const bool ok = assemble(spec, work, root, sizeof root, err, err_size) &&
                    install(root, destination, err, err_size);
    loader_stop(spinner);

    /* Whatever is left of the working tree goes, on success and on failure:
       remains that survive would be read as a source next time. */
    (void)fs_remove_tree(work);
    if(ok)
        snprintf(out, out_size, "%s", destination);
    return ok;
}

/* --- [[provide]] --- */

/* True when any segment of `path` is `..`. Lexical and cheap, so the common
   refusal happens before a syscall; the symlink half is resolve_inside below. */
static bool climbs_out(const char *path) {
    for(const char *at = path; *at != '\0';) {
        const char *slash = strchr(at, '/');
        const size_t length = slash == NULL ? strlen(at) : (size_t)(slash - at);
        if(length == 2 && at[0] == '.' && at[1] == '.')
            return true;
        if(slash == NULL)
            break;
        at = slash + 1;
    }
    return false;
}

/* `path` with its last segment removed, or "." when it has only one. */
static void parent_of(const char *path, char *out, size_t size) {
    const char *slash = strrchr(path, '/');
    if(slash == NULL) {
        snprintf(out, size, ".");
        return;
    }
    const size_t length = (size_t)(slash - path);
    snprintf(out, size, "%.*s", (int)(length == 0 ? 1 : length), length == 0 ? "/" : path);
}

/* Resolved, and under `root_real`. `path` must exist; a caller holding one that
   may not asks about its directory instead. */
static bool resolve_inside(const char *root_real, const char *path) {
    char real[SOURCE_PATH_MAX];
    if(!fs_real_path(path, real, sizeof real))
        return false;
    const size_t length = strlen(root_real);
    return strncmp(real, root_real, length) == 0 && (real[length] == '/' || real[length] == '\0');
}

/* One entry's path, joined onto the root and checked against it.
 *
 * `must_exist` says which of the two this is, and it decides what gets
 * resolved: the file itself for a `from`, which has to be there, and the
 * directory for a `file`, which is about to be created. Resolution is what
 * catches a symlink — a path spelling no `..` still lands elsewhere if a
 * directory along it points there. */
static bool provided_path(const char *root, const char *root_real, const char *relative,
                          const char *which, bool must_exist, size_t index, char *out,
                          size_t out_size, char *err, size_t err_size) {
    if(relative[0] == '\0')
        return fail_fmt(err, err_size, "[[provide]] #%zu names an empty '%s'", index + 1, which);
    if(fs_path_is_absolute(relative))
        return fail_fmt(err, err_size, "[[provide]] #%zu names an absolute '%s'", index + 1, which);
    if(climbs_out(relative))
        return fail_fmt(err, err_size,
                        "[[provide]] #%zu names a '%s' that climbs out of the source", index + 1,
                        which);
    if(!fs_format_path(out, out_size, "%s/%s", root, relative))
        return fail_fmt(err, err_size, "[[provide]] #%zu names a '%s' that is too long", index + 1,
                        which);

    char probe[SOURCE_PATH_MAX];
    if(must_exist)
        snprintf(probe, sizeof probe, "%s", out);
    else
        parent_of(out, probe, sizeof probe);

    /* Absent and outside are different answers and get different messages:
       reporting a missing file as an escape attempt sends the reader looking
       for a security problem that is not there. */
    if(!fs_path_exists(probe)) {
        if(must_exist)
            return fail_fmt(err, err_size,
                            "[[provide]] #%zu takes '%s' from the source, which has no such file",
                            index + 1, relative);
        /* A directory of its own for what it arranges — libpq's three public
           headers side by side — is made, once the nearest directory that
           does exist is known to be inside: a symlink along the way would
           otherwise carry the new directories outside the source. */
        char ancestor[SOURCE_PATH_MAX];
        snprintf(ancestor, sizeof ancestor, "%s", probe);
        while(!fs_path_exists(ancestor)) {
            char up[SOURCE_PATH_MAX];
            parent_of(ancestor, up, sizeof up);
            if(strcmp(up, ancestor) == 0)
                break;
            snprintf(ancestor, sizeof ancestor, "%s", up);
        }
        if(!resolve_inside(root_real, ancestor))
            return fail_fmt(err, err_size, "[[provide]] #%zu resolves its '%s' outside the source",
                            index + 1, which);
        if(!fs_make_dirs(probe))
            return fail_fmt(err, err_size, "[[provide]] #%zu could not make the directory for '%s'",
                            index + 1, relative);
    }
    if(!resolve_inside(root_real, probe))
        return fail_fmt(err, err_size, "[[provide]] #%zu resolves its '%s' outside the source",
                        index + 1, which);
    return true;
}

/* Byte for byte, without loading either into memory: a provision is repeated on
   every build, and answering "already done" has to be cheap and exact. */
static bool same_bytes(const char *left, const char *right) {
    FILE *a = fopen(left, "rb");
    FILE *b = fopen(right, "rb");
    bool same = a != NULL && b != NULL;
    while(same) {
        char left_chunk[4096];
        char right_chunk[4096];
        const size_t read_a = fread(left_chunk, 1, sizeof left_chunk, a);
        const size_t read_b = fread(right_chunk, 1, sizeof right_chunk, b);
        if(read_a != read_b || memcmp(left_chunk, right_chunk, read_a) != 0)
            same = false;
        else if(read_a == 0)
            break;
    }
    if(a != NULL)
        (void)fclose(a);
    if(b != NULL)
        (void)fclose(b);
    return same;
}

static bool copy_bytes(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if(in == NULL)
        return false;
    FILE *out = fopen(to, "wb");
    if(out == NULL) {
        (void)fclose(in);
        return false;
    }

    char chunk[4096];
    size_t read = 0;
    bool ok = true;
    while(ok && (read = fread(chunk, 1, sizeof chunk, in)) > 0)
        ok = fwrite(chunk, 1, read, out) == read;
    ok = ok && ferror(in) == 0;
    (void)fclose(in);
    return fclose(out) == 0 && ok;
}

static bool provide_one(const char *root, const char *root_real, const recipe_provision *entry,
                        size_t index, char *err, size_t err_size) {
    char from[SOURCE_PATH_MAX];
    char file[SOURCE_PATH_MAX];
    if(!provided_path(root, root_real, entry->from, "from", true, index, from, sizeof from, err,
                      err_size) ||
       !provided_path(root, root_real, entry->file, "file", false, index, file, sizeof file, err,
                      err_size))
        return false;

    if(fs_is_dir(from))
        return fail_fmt(err, err_size,
                        "[[provide]] #%zu takes '%s' from the source, which is a directory rather "
                        "than a file",
                        index + 1, entry->from);

    if(fs_path_exists(file)) {
        /* Already provided is not an error — a path origin is used where it
           lies and is provided again on every build. Anything else there is
           upstream's own file, and replacing it would be a patch. */
        if(same_bytes(file, from))
            return true;
        return fail_fmt(
            err, err_size,
            "[[provide]] #%zu would write '%s', which the source already contains with "
            "different bytes; a recipe completes a configuration and does not patch one",
            index + 1, entry->file);
    }

    if(!copy_bytes(from, file))
        return fail_fmt(err, err_size, "[[provide]] #%zu could not write '%s'", index + 1,
                        entry->file);
    return true;
}

bool source_provide(const char *root, const struct recipe_provide *provide, char *err,
                    size_t err_size) {
    if(provide == NULL || provide->count == 0)
        return true;

    char root_real[SOURCE_PATH_MAX];
    if(!fs_real_path(root, root_real, sizeof root_real))
        return fail_about(err, err_size, "the source cannot be resolved", root);

    for(size_t i = 0; i < provide->count; i++) {
        if(!provide_one(root, root_real, &provide->items[i], i, err, err_size))
            return false;
    }
    return true;
}

/* The tag `line` of an ls-remote listing names, or NULL: "<sha>\trefs/tags/<tag>". */
static const char *tag_of(const char *line, size_t length, char *tag, size_t size) {
    static const char prefix[] = "refs/tags/";
    const char *tab = memchr(line, '\t', length);
    if(tab == NULL)
        return NULL;
    const char *name = tab + 1;
    const size_t rest = length - (size_t)(name - line);
    if(rest <= sizeof prefix - 1 || strncmp(name, prefix, sizeof prefix - 1) != 0)
        return NULL;
    const size_t tag_length = rest - (sizeof prefix - 1);
    if(tag_length >= size)
        return NULL;
    memcpy(tag, name + sizeof prefix - 1, tag_length);
    tag[tag_length] = '\0';
    return tag;
}

bool source_newest_release_tag(const char *listing, char *out, size_t size) {
    bool found = false;
    semver newest = {0};
    for(const char *line = listing; *line != '\0';) {
        const char *end = strchr(line, '\n');
        const size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
        char tag[128];
        semver version;
        /* A release is a version, with or without the `v` tags usually carry,
           and not a pre-release: a new project starts on what was released. */
        if(tag_of(line, length, tag, sizeof tag) != NULL &&
           semver_parse(tag[0] == 'v' ? tag + 1 : tag, &version) && version.prerelease[0] == '\0' &&
           (!found || semver_compare(&version, &newest) > 0) && strlen(tag) < size) {
            newest = version;
            snprintf(out, size, "%s", tag);
            found = true;
        }
        line = end != NULL ? end + 1 : line + length;
    }
    return found;
}

bool source_git_newest_release(const char *url, char *out, size_t size, char *err,
                               size_t err_size) {
    if(url[0] == '-')
        return fail(err, err_size, "a git URL may not begin with '-'");
    const char *argv[] = {"git", "ls-remote", "--tags", "--refs", url, NULL};
    char captured[8192] = "";
    const int code = process_capture(argv, captured, sizeof captured);
    if(code == 127)
        return fail(err, err_size, "git is not installed, and molto needs it to ask for a release");
    if(code != 0)
        return fail_about(err, err_size, "could not reach the repository", url);
    if(!source_newest_release_tag(captured, out, size))
        return fail_about(err, err_size, "the repository tags no release", url);
    return true;
}
