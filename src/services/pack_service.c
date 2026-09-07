#include <molto/services/pack_service.h>

#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>

#include <stdio.h>
#include <string.h>

/* What each packing is called. */
#define FORMAT_ZSTD "tar.zst"
#define FORMAT_GZIP "tar.gz"

/* zstd at 19 and gzip at 9: an artifact is packed once and downloaded for
   years, so the asymmetry is the point of spending the time. `-n` keeps a date
   out of the gzip stream and `-T0` lets zstd use the cores it is given. */
#define ZSTD_FILTER "zstd -19 -T0"
#define GZIP_FILTER "gzip -9 -n"

static bool fail(char *err, size_t err_size, const char *message) {
    if(err != NULL && err_size > 0)
        snprintf(err, err_size, "%s", message);
    return false;
}

const char *pack_default_format(const char *target) {
    return target != NULL && strncmp(target, "windows-", 8) == 0 ? FORMAT_GZIP : FORMAT_ZSTD;
}

bool pack_format_is_known(const char *format) {
    return format != NULL &&
           (strcmp(format, FORMAT_ZSTD) == 0 || strcmp(format, FORMAT_GZIP) == 0);
}

bool pack_archive_name(const char *name, const char *version, const char *target,
                       const char *format, char *out, size_t size) {
    const int written = snprintf(out, size, "%s-%s-%s.%s", name, version, target, format);
    return written > 0 && (size_t)written < size;
}

/*
 * Which tar is going to run, asked rather than assumed.
 *
 * Not the user's choice, and on Windows not even the shell's: molto starts tar
 * through CreateProcess with a bare name, and that search reaches the system
 * directory before it reaches PATH. So `C:\Windows\System32\tar.exe` is what
 * runs whatever else is installed and whichever shell the command came from —
 * bsdtar 3.5.2, libarchive linked against zlib and nothing else.
 *
 * The two implementations do not share a spelling for any of the three things
 * that matter here. GNU tar sorts with `--sort=name`, flattens ownership with
 * `--owner=0 --group=0`, and compresses by handing the stream to a program
 * with `-I`. bsdtar has none of those: it takes `--uid/--gid/--uname/--gname`,
 * has no way to sort at all, and compresses with codecs compiled into it.
 *
 * Paired with `--version` so each question costs nothing: tar parses its
 * options, finds nothing to do, and says whether it recognised the flag.
 */
static bool tar_takes(const char *flag) {
    const char *argv[] = {"tar", flag, "--version", NULL};
    char captured[256] = "";
    /* Both streams, because the answer is on the second one: a tar that does
       not know the flag prints its usage to stderr, and capturing only stdout
       leaves that on the terminal — where it reads as a failed publish rather
       than as a question being asked. */
    return process_capture_all(argv, NULL, 0, captured, sizeof captured, NULL) == 0;
}

/* GNU tar is the one that can sort, and sorting is the only property that
   cannot be had another way. Asked once. */
static bool tar_is_gnu(void) {
    static bool checked = false;
    static bool gnu = false;
    if(!checked) {
        gnu = tar_takes("--sort=name");
        checked = true;
    }
    return gnu;
}

/* Whether the outside compressor GNU tar would delegate to is installed. Not
   asked of bsdtar, which delegates to nobody. */
static bool filter_available(const char *format, char *err, size_t err_size) {
    const char *program = strcmp(format, FORMAT_GZIP) == 0 ? "gzip" : "zstd";
    const char *argv[] = {program, "--version", NULL};

    char captured[256] = "";
    const int code = process_capture_all(argv, NULL, 0, captured, sizeof captured, NULL);
    if(code != 0) {
        char message[256];
        snprintf(message, sizeof message, "%s is needed to pack a %s here and does not run",
                 program, format);
        return fail(err, err_size, message);
    }
    return true;
}

#define ARGV_MAX 20

/*
 * `tar -c` for the tar that is actually going to run.
 *
 * Both branches put the contents at the root of the archive with `-C <dir> .`
 * rather than naming the directory, because pickup extracts with
 * `--strip-components=0` and expects `bin/` to be the first thing it finds.
 */
static size_t build_gnu(const char *directory, const char *archive, const char *format,
                        const char *argv[ARGV_MAX]) {
    size_t n = 0;
    argv[n++] = "tar";
    /* A fixed entry order rather than the filesystem's, and nobody's uid on
       the way out: a published coordinate cannot be replaced, so the only
       check anyone can make on an artifact later is to pack the same tree
       again and compare — which is a check only if the bytes repeat. */
    argv[n++] = "--sort=name";
    argv[n++] = "--owner=0";
    argv[n++] = "--group=0";
    argv[n++] = "--numeric-owner";
    argv[n++] = "-C";
    argv[n++] = directory;
    argv[n++] = "-c";
    argv[n++] = "-I";
    argv[n++] = strcmp(format, FORMAT_GZIP) == 0 ? GZIP_FILTER : ZSTD_FILTER;
    argv[n++] = "-f";
    argv[n++] = archive;
    argv[n++] = ".";
    argv[n] = NULL;
    return n;
}

static size_t build_bsd(const char *directory, const char *archive, const char *argv[ARGV_MAX]) {
    size_t n = 0;
    argv[n++] = "tar";
    /* What this tar offers of the same idea. It cannot sort, which is said out
       loud by pack_directory rather than quietly not done. */
    argv[n++] = "--uid=0";
    argv[n++] = "--gid=0";
    argv[n++] = "--uname=";
    argv[n++] = "--gname=";
    argv[n++] = "--numeric-owner";
    argv[n++] = "-C";
    argv[n++] = directory;
    /* The codec is compiled in; there is no program to name and no `-I` to
       name it with. */
    argv[n++] = "-c";
    argv[n++] = "-z";
    argv[n++] = "-f";
    argv[n++] = archive;
    argv[n++] = ".";
    argv[n] = NULL;
    return n;
}

bool pack_directory(const char *directory, const char *archive, const char *format, char *err,
                    size_t err_size) {
    if(!pack_format_is_known(format))
        return fail(err, err_size, "molto packs tar.zst and tar.gz, and was asked for neither");
    if(!fs_is_dir(directory))
        return fail(err, err_size, "what was named to pack is not a directory");

    const char *argv[ARGV_MAX];
    if(tar_is_gnu()) {
        if(!filter_available(format, err, err_size))
            return false;
        (void)build_gnu(directory, archive, format, argv);
    } else {
        /* The tar Windows ships has zlib and no more of libarchive's codecs
           than that, so gzip is the only packing it can write. That is not the
           constraint it sounds like: a windows-* target packs as gzip anyway,
           because it is also the only packing that tar can *read*. Any other
           target wants zstd, and wanting it here means GNU tar. */
        if(strcmp(format, FORMAT_GZIP) != 0)
            return fail(err, err_size,
                        "this tar writes gzip and nothing else; packing a tar.zst here needs GNU "
                        "tar on the PATH");
        (void)build_bsd(directory, archive, argv);
    }

    const int code = process_run(argv);
    if(code == 127)
        return fail(err, err_size, "tar is needed to pack an artifact and is not on the PATH");
    if(code != 0) {
        /* What tar was part-way through writing is not an artifact, and must
           not be left where the next step would find it and publish it. */
        (void)remove(archive);
        return fail(err, err_size, "tar could not pack the directory");
    }
    if(!fs_path_exists(archive))
        return fail(err, err_size, "tar reported success and wrote no archive");
    return true;
}

bool pack_is_reproducible(void) { return tar_is_gnu(); }
