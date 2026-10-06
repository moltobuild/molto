#include <molto/services/platform_service.h>

#include <molto/services/fs_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * The parts of four package formats molto reads itself (RFC-0022).
 *
 * Not the whole of any of them. A .deb is an `ar` archive and molto needs one
 * member's offset; an rpm is two headers molto skips and a cpio it unpacks; a
 * Mach-O is a list of load commands of which molto rewrites one kind. Each is
 * a page of code, which is less than any library that reads them would cost to
 * keep patched — and every one of these is a format whose bytes arrived from
 * the network, so each read is bounded by the buffer and checked before it is
 * trusted.
 */

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

/* --- ar: a .deb --- */

#define AR_MAGIC "!<arch>\n"
#define AR_MAGIC_SIZE 8
#define AR_HEADER_SIZE 60
#define AR_NAME_SIZE 16
#define AR_SIZE_AT 48
#define AR_SIZE_SIZE 10

bool platform_ar_member(const unsigned char *data, size_t size, const char *prefix, size_t *offset,
                        size_t *length) {
    if(size < AR_MAGIC_SIZE || memcmp(data, AR_MAGIC, AR_MAGIC_SIZE) != 0)
        return false;

    size_t at = AR_MAGIC_SIZE;
    while(at + AR_HEADER_SIZE <= size) {
        const unsigned char *header = data + at;
        char digits[AR_SIZE_SIZE + 1];
        memcpy(digits, header + AR_SIZE_AT, AR_SIZE_SIZE);
        digits[AR_SIZE_SIZE] = '\0';
        char *end = NULL;
        const unsigned long long member = strtoull(digits, &end, 10);
        if(end == digits || member > size - at - AR_HEADER_SIZE)
            return false;

        if(strncmp((const char *)header, prefix, strlen(prefix)) == 0 &&
           strlen(prefix) <= AR_NAME_SIZE) {
            *offset = at + AR_HEADER_SIZE;
            *length = (size_t)member;
            return true;
        }
        /* Members are aligned to two bytes. */
        at += AR_HEADER_SIZE + (size_t)member + (size_t)(member & 1u);
    }
    return false;
}

/* --- rpm --- */

#define RPM_LEAD_SIZE 96
#define RPM_HEADER_PREAMBLE 16
#define RPM_INDEX_ENTRY 16

static uint32_t big_endian32(const unsigned char *at) {
    return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | (uint32_t)at[3];
}

/* Where a header that starts at `at` ends. */
static bool rpm_header_end(const unsigned char *data, size_t size, size_t at, size_t *end) {
    static const unsigned char MAGIC[] = {0x8e, 0xad, 0xe8};
    if(at > size || size - at < RPM_HEADER_PREAMBLE || memcmp(data + at, MAGIC, sizeof MAGIC) != 0)
        return false;
    const uint64_t entries = big_endian32(data + at + 8);
    const uint64_t store = big_endian32(data + at + 12);
    const uint64_t length = RPM_HEADER_PREAMBLE + entries * RPM_INDEX_ENTRY + store;
    if(length > size - at)
        return false;
    *end = at + (size_t)length;
    return true;
}

bool platform_rpm_payload(const unsigned char *data, size_t size, size_t *offset) {
    static const unsigned char LEAD[] = {0xed, 0xab, 0xee, 0xdb};
    if(size < RPM_LEAD_SIZE || memcmp(data, LEAD, sizeof LEAD) != 0)
        return false;

    size_t at = 0;
    if(!rpm_header_end(data, size, RPM_LEAD_SIZE, &at))
        return false;
    /* The signature header is padded to a multiple of eight; the main header
       is not, and the payload follows it directly. */
    at = (at + 7u) & ~(size_t)7u;
    if(!rpm_header_end(data, size, at, &at) || at >= size)
        return false;
    *offset = at;
    return true;
}

/* --- cpio (newc), an rpm's payload --- */

#define CPIO_HEADER_SIZE 110
#define CPIO_FIELD_SIZE 8
#define CPIO_TRAILER "TRAILER!!!"

/* Field `index` of a newc header: eight hex digits after the six-byte magic. */
static bool cpio_field(const unsigned char *header, size_t index, uint32_t *out) {
    uint32_t value = 0;
    for(size_t i = 0; i < CPIO_FIELD_SIZE; i++) {
        const unsigned char c = header[6 + index * CPIO_FIELD_SIZE + i];
        value <<= 4;
        if(c >= '0' && c <= '9')
            value |= (uint32_t)(c - '0');
        else if(c >= 'a' && c <= 'f')
            value |= (uint32_t)(c - 'a' + 10);
        else if(c >= 'A' && c <= 'F')
            value |= (uint32_t)(c - 'A' + 10);
        else
            return false;
    }
    *out = value;
    return true;
}

static size_t align4(size_t value) { return (value + 3u) & ~(size_t)3u; }

/* A member's name, made relative: "./usr/include" and "/usr/include" both
   become "usr/include". Refused when any segment is `..`. */
static bool member_path(const char *name, char *out, size_t size) {
    while(name[0] == '.' && name[1] == '/')
        name += 2;
    while(name[0] == '/')
        name++;
    for(const char *at = name; *at != '\0';) {
        const size_t length = strcspn(at, "/");
        if(length == 2 && at[0] == '.' && at[1] == '.')
            return false;
        at += length;
        if(*at == '/')
            at++;
    }
    const int written = snprintf(out, size, "%s", name);
    return written >= 0 && (size_t)written < size;
}

/* True when the directory `path` would be written into resolves under
   `root_real`. A symlink unpacked earlier can point anywhere, and writing a
   later member through it is how an archive escapes the directory it was
   unpacked into. */
static bool parent_inside(const char *path, const char *root_real) {
    char parent[PLATFORM_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if(slash == NULL)
        return false;
    *slash = '\0';
    char real[PLATFORM_PATH_MAX];
    if(!fs_real_path(parent, real, sizeof real))
        return false;
    const size_t length = strlen(root_real);
    return strncmp(real, root_real, length) == 0 && (real[length] == '/' || real[length] == '\0');
}

static bool parent_dirs(const char *path) {
    char parent[PLATFORM_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if(slash == NULL)
        return true;
    *slash = '\0';
    return fs_make_dirs(parent);
}

static bool write_member(const char *path, const unsigned char *data, size_t size, uint32_t mode) {
    FILE *file = fopen(path, "wb");
    if(file == NULL)
        return false;
    const bool ok = fwrite(data, 1, size, file) == size;
    if(fclose(file) != 0 || !ok)
        return false;
    (void)chmod(path, (mode_t)(mode & 0755u) | 0200u);
    return true;
}

bool platform_cpio_unpack(const unsigned char *data, size_t size, const char *root, char *err,
                          size_t err_size) {
    if(!fs_make_dirs(root))
        return fail(err, err_size, "could not create %s", root);
    char root_real[PLATFORM_PATH_MAX];
    if(!fs_real_path(root, root_real, sizeof root_real))
        return fail(err, err_size, "could not resolve %s", root);

    size_t at = 0;
    for(;;) {
        if(size - at < CPIO_HEADER_SIZE ||
           (memcmp(data + at, "070701", 6) != 0 && memcmp(data + at, "070702", 6) != 0))
            return fail(err, err_size, "an rpm payload that is not a newc cpio archive");
        const unsigned char *header = data + at;
        uint32_t mode = 0;
        uint32_t file_size = 0;
        uint32_t name_size = 0;
        if(!cpio_field(header, 1, &mode) || !cpio_field(header, 6, &file_size) ||
           !cpio_field(header, 11, &name_size) || name_size == 0)
            return fail(err, err_size, "a cpio header molto cannot read");

        const size_t name_at = at + CPIO_HEADER_SIZE;
        if(name_size > size - name_at || data[name_at + name_size - 1] != '\0')
            return fail(err, err_size, "a cpio member whose name runs past the archive");
        const char *name = (const char *)data + name_at;
        const size_t body_at = align4(name_at + name_size);
        if(body_at > size || file_size > size - body_at)
            return fail(err, err_size, "a cpio member whose contents run past the archive");
        const unsigned char *body = data + body_at;
        at = align4(body_at + file_size);

        if(strcmp(name, CPIO_TRAILER) == 0)
            return true;

        char relative[PLATFORM_PATH_MAX];
        if(!member_path(name, relative, sizeof relative))
            return fail(err, err_size, "the cpio member '%s' climbs out of what it unpacks into",
                        name);
        if(relative[0] == '\0' || strcmp(relative, ".") == 0)
            continue;
        char path[PLATFORM_PATH_MAX];
        if(!fs_format_path(path, sizeof path, "%s/%s", root, relative))
            return fail(err, err_size, "the cpio member '%s' is too long", name);

        const uint32_t kind = mode & 0170000u;
        if(kind == 0040000u) {
            if(!fs_make_dirs(path))
                return fail(err, err_size, "could not create %s", path);
            continue;
        }
        if(kind != 0100000u && kind != 0120000u)
            continue; /* devices, fifos and sockets have no place in a cache */
        if(!parent_dirs(path) || !parent_inside(path, root_real))
            return fail(err, err_size, "the cpio member '%s' would land outside %s", name, root);
        (void)remove(path);

        if(kind == 0120000u) {
            char target[PLATFORM_PATH_MAX];
            if(file_size >= sizeof target)
                return fail(err, err_size, "the symlink '%s' is too long", name);
            memcpy(target, body, file_size);
            target[file_size] = '\0';
            if(!fs_link(target, path))
                return fail(err, err_size, "could not create the symlink %s", path);
        } else if(!write_member(path, body, file_size, mode)) {
            return fail(err, err_size, "could not write %s", path);
        }
    }
}

/* --- Mach-O: relocating a bottle --- */

#define MH_MAGIC_64 0xfeedfacfu
#define MACHO_HEADER_64 32
#define LC_REQ_DYLD 0x80000000u
#define LC_LOAD_DYLIB 0x0cu
#define LC_ID_DYLIB 0x0du
#define LC_LAZY_LOAD_DYLIB 0x20u
#define LC_LOAD_WEAK_DYLIB (0x18u | LC_REQ_DYLD)
#define LC_REEXPORT_DYLIB (0x1fu | LC_REQ_DYLD)
#define LC_LOAD_UPWARD_DYLIB (0x23u | LC_REQ_DYLD)
#define DYLIB_NAME_OFFSET 8 /* dylib_command: cmd, cmdsize, then the name's offset */

#define HOMEBREW_PREFIX "@@HOMEBREW_PREFIX@@"
#define HOMEBREW_CELLAR "@@HOMEBREW_CELLAR@@"
#define RPATH_PREFIX "@rpath/"

static uint32_t little_endian32(const unsigned char *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static bool names_a_dylib(uint32_t command) {
    return command == LC_LOAD_DYLIB || command == LC_ID_DYLIB || command == LC_LAZY_LOAD_DYLIB ||
           command == LC_LOAD_WEAK_DYLIB || command == LC_REEXPORT_DYLIB ||
           command == LC_LOAD_UPWARD_DYLIB;
}

static bool is_placeholder(const char *name, size_t room) {
    const size_t prefix = sizeof HOMEBREW_PREFIX - 1;
    return room > prefix && (strncmp(name, HOMEBREW_PREFIX, prefix) == 0 ||
                             strncmp(name, HOMEBREW_CELLAR, prefix) == 0);
}

/* One dylib command's name, rewritten to @rpath/<file> when it is a
   placeholder. The new name is always shorter — a placeholder alone is longer
   than "@rpath/" — so it fits where the old one was, and what is left of the
   command is zeroed as the linker would have left it. */
static bool rewrite_name(unsigned char *command, uint32_t command_size, bool *changed) {
    if(command_size < DYLIB_NAME_OFFSET + 4)
        return false;
    const uint32_t offset = little_endian32(command + DYLIB_NAME_OFFSET);
    if(offset >= command_size)
        return false;
    char *name = (char *)command + offset;
    const size_t room = command_size - offset;
    if(memchr(name, '\0', room) == NULL)
        return false;
    if(!is_placeholder(name, room))
        return true;

    const char *slash = strrchr(name, '/');
    const char *file = slash == NULL ? name : slash + 1;
    char renamed[PLATFORM_PATH_MAX];
    const int written = snprintf(renamed, sizeof renamed, RPATH_PREFIX "%s", file);
    if(written < 0 || (size_t)written >= room)
        return false;
    memset(name, 0, room);
    memcpy(name, renamed, (size_t)written);
    *changed = true;
    return true;
}

bool platform_macho_relocate(unsigned char *data, size_t size, bool *changed) {
    *changed = false;
    if(size < MACHO_HEADER_64 || little_endian32(data) != MH_MAGIC_64)
        return false;
    const uint32_t commands = little_endian32(data + 16);
    const uint32_t commands_size = little_endian32(data + 20);
    if(commands_size > size - MACHO_HEADER_64)
        return false;

    size_t at = MACHO_HEADER_64;
    const size_t end = MACHO_HEADER_64 + commands_size;
    for(uint32_t i = 0; i < commands; i++) {
        if(end - at < 8)
            return false;
        const uint32_t command = little_endian32(data + at);
        const uint32_t command_size = little_endian32(data + at + 4);
        if(command_size < 8 || command_size > end - at)
            return false;
        if(names_a_dylib(command) && !rewrite_name(data + at, command_size, changed))
            return false;
        at += command_size;
    }
    return true;
}
