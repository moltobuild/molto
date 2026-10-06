#ifndef MOLTO_PLATFORM_SERVICE_H
#define MOLTO_PLATFORM_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <molto/util/doc.h>
#include <molto/util/str_list.h>

/*
 * A platform recipe (RFC-0022): a library every platform already packages,
 * named file by file in that platform's own repository.
 *
 * GTK is the case. Building it is RFC-0009's `via = "delegate"`, re-hosting it
 * makes the registry a redistributor of binaries it did not build, and on a
 * stock desktop the host has its runtime but not its headers. What every
 * platform does have is a repository publishing GTK — Ubuntu's .debs, Fedora's
 * .rpms, Arch's and MSYS2's pacman packages, Homebrew's bottles — and this
 * service downloads those files with curl, verifies each digest, and unpacks
 * them itself. No package manager runs, and nothing inside any package does:
 * maintainer scripts, scriptlets and .INSTALL hooks are never read.
 *
 * The host is asked first. A recipe naming `[host] pkgconfig` is answered by
 * RFC-0016's resolver when the host has the library with its headers, and
 * nothing is downloaded at all.
 */

/* Room enough for GTK on Windows — 84 packages — with headroom for a toolkit
   twice its size, and small enough that a recipe naming thousands is refused
   rather than fetched. */
#define PLATFORM_MAX 16
#define PLATFORM_MAX_FILES 256
#define PLATFORM_FIELD_MAX 64
#define PLATFORM_PATH_MAX 1024

typedef enum {
    platform_format_deb,    /* an `ar` archive holding data.tar.* */
    platform_format_rpm,    /* a lead, two headers and a compressed cpio */
    platform_format_pacman, /* a tarball, zstd or xz: Arch and MSYS2 */
    platform_format_bottle, /* a Homebrew bottle: tar.gz of <formula>/<version>/ */
} platform_format;

/* Whose libraries a program loads. */
typedef enum {
    /* The files are the development half; the runtime is the host's, and
       `link` names it by soname. Linux, where GTK comes with the desktop and a
       cached Mesa would fight the host's GPU driver. */
    platform_runtime_host,
    /* The files are the whole closure, runtime included: `lib` is an rpath and
       `bin` is copied beside an executable. Windows and macOS. */
    platform_runtime_bundled,
} platform_runtime;

typedef struct {
    char name[PLATFORM_FIELD_MAX];
    char os[PLATFORM_FIELD_MAX];   /* linux, macos, windows */
    char arch[PLATFORM_FIELD_MAX]; /* x86_64, aarch64 */
    char distro[PLATFORM_FIELD_MAX];
    char distro_version[PLATFORM_FIELD_MAX];
    char os_version_min[PLATFORM_FIELD_MAX];
    char upstream[PLATFORM_FIELD_MAX];
    platform_format format;
    platform_runtime runtime;
    str_list include; /* relative to the unpacked root */
    str_list link;    /* as -l takes them; ":soname" names one file */
    str_list lib;     /* relative; -L, and the rpath when bundled */
    str_list bin;     /* relative; DLLs copied beside an executable */
    str_list urls;
    str_list digests; /* lowercase hex sha256, one per url */
} platform_entry;

typedef struct {
    /* The pkg-config module asked of the host first; "" asks nothing. */
    char pkgconfig[PLATFORM_FIELD_MAX];
    platform_entry *items;
    size_t count;
} platform_recipe;

void platform_recipe_init(platform_recipe *recipe);
void platform_recipe_free(platform_recipe *recipe);

/* Read `[host]` and every `[[platform]]` of a recipe whose form is `platform`,
   from TOML on disk or JSON a registry served. Every refusal names the
   platform: a missing name, os, arch, format or runtime; a value outside its
   vocabulary; a path that is absolute or climbs out with `..`; a file without
   a digest; two platforms with one name; more than the limits above; and any
   of `[source]`, `[build]`, `[[provide]]`, `[artifacts]` or `[deps]` beside
   them, which would be two answers to one question. */
[[nodiscard]] bool platform_recipe_read(doc_view doc, platform_recipe *out, char *err,
                                        size_t err_size);

/* One digest over every platform's pinned files, for the lock: the same on
   every machine whichever platform each of them chooses, and different the
   moment any pin is. */
void platform_recipe_digest(const platform_recipe *recipe, char hex_out[65]);

/* --- which platform this machine is --- */

typedef struct {
    char os[PLATFORM_FIELD_MAX];
    char arch[PLATFORM_FIELD_MAX];
    char distro[PLATFORM_FIELD_MAX];         /* Linux: os-release ID */
    char distro_version[PLATFORM_FIELD_MAX]; /* Linux: os-release VERSION_ID */
    char os_version[PLATFORM_FIELD_MAX];     /* macOS: the product version */
} platform_host;

/* What molto was compiled for, plus what the system says about itself. */
void platform_host_detect(platform_host *out);

/* `ID` and `VERSION_ID` out of an os-release file's text. Quotes are removed.
   Split out of detection so it can be tested without a Linux. */
void platform_host_read_os_release(const char *text, platform_host *out);

/* The first entry `host` matches, in recipe order. False, with a message that
   lists every platform the recipe does serve, when none does. */
[[nodiscard]] bool platform_choose(const platform_recipe *recipe, const platform_host *host,
                                   size_t *index, char *err, size_t err_size);

/* -1, 0 or 1 as dotted version `a` is older, the same as or newer than `b`,
   compared number by number: "15.0" < "15.4" < "26". */
[[nodiscard]] int platform_version_compare(const char *a, const char *b);

/* --- what a build gets --- */

typedef struct {
    /* "pkg-config" when the host answered, or the platform's name. */
    char answered[PLATFORM_FIELD_MAX];
    /* What the platform calls the version it supplied; "" if unknown. */
    char upstream[PLATFORM_FIELD_MAX];
    /* Where the unpacked files are, absolute. An empty directory in the cache
       when the host answered: a dependency still has a root to name. */
    char root[PLATFORM_PATH_MAX];
    str_list includes; /* -I directories, absolute */
    str_list links;    /* link-line flags, verbatim: -l…, -L…, -Wl,-rpath,… */
    /* Directories whose DLLs belong beside an executable (Windows). */
    str_list runtime_dirs;
    /* Directories outside molto's cache this answer may name: the host's own,
       when it answered. A document is held to them (RFC-0013), and they come
       from here rather than from the document so nothing a producer wrote can
       widen them. */
    str_list bounds;
} platform_answer;

void platform_answer_init(platform_answer *answer);
void platform_answer_free(platform_answer *answer);

/* Ask the host, and when it has no answer, fetch the platform this machine
   matches into `<cache>/platforms/<name>/<version>/<platform>` and describe it.
   A coordinate already fetched is not fetched again. */
[[nodiscard]] bool platform_resolve(const platform_recipe *recipe, const char *name,
                                    const char *version, platform_answer *out, char *err,
                                    size_t err_size);

/* --- the formats, exposed for the tests that pin them --- */

/* The member of an `ar` archive whose name starts with `prefix` — a .deb's
   `data.tar` — as an offset and a length into `data`. */
[[nodiscard]] bool platform_ar_member(const unsigned char *data, size_t size, const char *prefix,
                                      size_t *offset, size_t *length);

/* Where an rpm's compressed payload starts: after the 96-byte lead, the
   signature header padded to eight bytes, and the main header. */
[[nodiscard]] bool platform_rpm_payload(const unsigned char *data, size_t size, size_t *offset);

/* Unpack a `newc` cpio archive into `root`: directories, regular files and
   symlinks, nothing else. A member that would land outside `root` is
   refused. */
[[nodiscard]] bool platform_cpio_unpack(const unsigned char *data, size_t size, const char *root,
                                        char *err, size_t err_size);

/* Rewrite every Homebrew placeholder install name in a 64-bit Mach-O to
   `@rpath/<file>`, in place: LC_ID_DYLIB and every LC_*_DYLIB that names one.
   `*changed` says whether anything was rewritten, which is what decides
   whether the file needs signing again. False for a file that is not a
   64-bit Mach-O or whose load commands do not fit inside it. A universal
   binary is not one this handles, and Homebrew bottles never are. */
[[nodiscard]] bool platform_macho_relocate(unsigned char *data, size_t size, bool *changed);

/* Copy every DLL under `dirs` into `destination` unless one there is already
   at least as new. For Windows, which looks for a program's DLLs beside it. */
[[nodiscard]] bool platform_copy_runtime(const str_list *dirs, const char *destination, char *err,
                                         size_t err_size);

#endif /* MOLTO_PLATFORM_SERVICE_H */
