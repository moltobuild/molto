#include <moltest.h>

#include <molto/services/fs_service.h>
#include <molto/services/platform_service.h>
#include <molto/services/recipe_service.h>
#include <molto/util/doc.h>
#include <molto/util/str_list.h>
#include <molto/util/toml.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Platform recipes (RFC-0022): how one is read and refused, which platform a
 * machine gets, and the four formats molto unpacks itself. Nothing here goes
 * to the network — the formats are built byte by byte below, which is also
 * what pins down exactly which bytes each reader trusts.
 */

#define DIGEST_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define DIGEST_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static const char *const RECIPE =
    "schema = 3\n"
    "form = \"platform\"\n"
    "kind = \"package\"\n"
    "name = \"gtk\"\n"
    "version = \"4.14.0\"\n"
    "target = \"any\"\n"
    "\n"
    "[host]\n"
    "pkgconfig = \"gtk4\"\n"
    "\n"
    "[[platform]]\n"
    "name = \"ubuntu-24.04-x86_64\"\n"
    "os = \"linux\"\n"
    "arch = \"x86_64\"\n"
    "distro = \"ubuntu\"\n"
    "distro_version = \"24.04\"\n"
    "format = \"deb\"\n"
    "runtime = \"host\"\n"
    "upstream = \"4.14.5\"\n"
    "include = [\"usr/include/gtk-4.0\"]\n"
    "link = [\":libgtk-4.so.1\"]\n"
    "\n"
    "[[platform.file]]\n"
    "url = \"https://example.org/libgtk-4-dev.deb\"\n"
    "sha256 = \"" DIGEST_A "\"\n"
    "\n"
    "[[platform]]\n"
    "name = \"arch-x86_64\"\n"
    "os = \"linux\"\n"
    "arch = \"x86_64\"\n"
    "distro = \"arch\"\n"
    "format = \"pacman\"\n"
    "runtime = \"host\"\n"
    "include = [\"usr/include/gtk-4.0\"]\n"
    "link = [\":libgtk-4.so.1\"]\n"
    "\n"
    "[[platform.file]]\n"
    "url = \"https://example.org/gtk4.pkg.tar.zst\"\n"
    "sha256 = \"" DIGEST_A "\"\n"
    "\n"
    "[[platform]]\n"
    "name = \"macos-arm64\"\n"
    "os = \"macos\"\n"
    "arch = \"aarch64\"\n"
    "os_version_min = \"15.0\"\n"
    "format = \"bottle\"\n"
    "runtime = \"bundled\"\n"
    "include = [\"include/gtk-4.0\"]\n"
    "link = [\"gtk-4\"]\n"
    "lib = [\"lib\"]\n"
    "\n"
    "[[platform.file]]\n"
    "url = \"https://ghcr.io/v2/homebrew/core/gtk4/blobs/sha256:" DIGEST_B "\"\n"
    "sha256 = \"" DIGEST_B "\"\n"
    "\n"
    "[[platform.file]]\n"
    "url = \"https://ghcr.io/v2/homebrew/core/glib/blobs/sha256:" DIGEST_A "\"\n"
    "sha256 = \"" DIGEST_A "\"\n";

/* Parse `text` and read it as a platform recipe. The document is freed before
   returning: everything the reader keeps is a copy. */
static bool read_recipe(const char *text, platform_recipe *out, char *err, size_t err_size) {
    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    if(doc == NULL) {
        snprintf(err, err_size, "not TOML: %s", parse_err);
        platform_recipe_init(out);
        return false;
    }
    const bool ok = platform_recipe_read(doc_from_toml(doc), out, err, err_size);
    toml_free(doc);
    return ok;
}

/* The canonical recipe with one line replaced, for each refusal below. */
static bool read_with(const char *find, const char *replace, char *err, size_t err_size) {
    char text[8192];
    const char *at = strstr(RECIPE, find);
    if(at == NULL)
        return true; /* a test whose anchor is gone fails on the assertion */
    snprintf(text, sizeof text, "%.*s%s%s", (int)(at - RECIPE), RECIPE, replace,
             at + strlen(find));
    platform_recipe recipe;
    const bool ok = read_recipe(text, &recipe, err, err_size);
    platform_recipe_free(&recipe);
    return ok;
}

/* --- reading --- */

DESCRIBE(a_platform_recipe_reads_every_platform_and_its_files) {
    platform_recipe recipe;
    char err[512] = "";
    ASSERT_TRUE(read_recipe(RECIPE, &recipe, err, sizeof err));

    EXPECT_STREQ("gtk4", recipe.pkgconfig);
    ASSERT_EQ(3u, recipe.count);

    const platform_entry *ubuntu = &recipe.items[0];
    EXPECT_STREQ("ubuntu-24.04-x86_64", ubuntu->name);
    EXPECT_STREQ("ubuntu", ubuntu->distro);
    EXPECT_STREQ("24.04", ubuntu->distro_version);
    EXPECT_STREQ("4.14.5", ubuntu->upstream);
    EXPECT_EQ(platform_format_deb, ubuntu->format);
    EXPECT_EQ(platform_runtime_host, ubuntu->runtime);
    EXPECT_STREQ(":libgtk-4.so.1", str_list_get(&ubuntu->link, 0));

    const platform_entry *mac = &recipe.items[2];
    EXPECT_EQ(platform_format_bottle, mac->format);
    EXPECT_EQ(platform_runtime_bundled, mac->runtime);
    EXPECT_STREQ("15.0", mac->os_version_min);
    ASSERT_EQ(2u, str_list_count(&mac->urls));
    EXPECT_STREQ(DIGEST_B, str_list_get(&mac->digests, 0));
    EXPECT_STREQ("lib", str_list_get(&mac->lib, 0));

    platform_recipe_free(&recipe);
}

/* A URL is a promise about a location: without a digest, a mirror that changed
   is a build that changed. */
DESCRIBE(a_platform_file_without_a_digest_is_refused) {
    char err[512] = "";
    EXPECT_FALSE(read_with("sha256 = \"" DIGEST_B "\"\n", "", err, sizeof err));
    EXPECT_TRUE(strstr(err, "sha256") != NULL);
    EXPECT_FALSE(read_with("sha256 = \"" DIGEST_B "\"\n", "sha256 = \"ABC\"\n", err, sizeof err));
}

DESCRIBE(a_platform_file_fetched_without_tls_is_refused) {
    char err[512] = "";
    EXPECT_FALSE(read_with("https://example.org/libgtk", "http://example.org/libgtk", err,
                           sizeof err));
    EXPECT_TRUE(strstr(err, "https") != NULL);
}

/* Every path a platform names is joined onto what it unpacks, so one that left
   it would put a directory the recipe's author chose on a consumer's command
   line. */
DESCRIBE(a_platform_path_that_leaves_the_root_is_refused) {
    char err[512] = "";
    EXPECT_FALSE(read_with("include = [\"usr/include/gtk-4.0\"]",
                           "include = [\"usr/../../etc\"]", err, sizeof err));
    EXPECT_TRUE(strstr(err, "stays inside") != NULL);
    EXPECT_FALSE(read_with("lib = [\"lib\"]", "lib = [\"/usr/lib\"]", err, sizeof err));
}

/* A library name becomes `-l<name>`; one that is a flag is a second option
   smuggled in through the first. */
DESCRIBE(a_platform_library_that_is_a_flag_is_refused) {
    char err[512] = "";
    EXPECT_FALSE(read_with("link = [\"gtk-4\"]", "link = [\"-Wl,-rpath,/tmp\"]", err, sizeof err));
    EXPECT_TRUE(strstr(err, "not a library name") != NULL);
}

DESCRIBE(a_platform_recipe_carrying_another_forms_tables_is_refused) {
    char err[512] = "";
    EXPECT_FALSE(read_with("[host]\n",
                           "[source]\narchive = \"https://example.org/x.tar.gz\"\n\n[host]\n", err,
                           sizeof err));
    EXPECT_TRUE(strstr(err, "[source]") != NULL);
}

DESCRIBE(two_platforms_with_one_name_are_refused) {
    char err[512] = "";
    EXPECT_FALSE(read_with("name = \"arch-x86_64\"", "name = \"macos-arm64\"", err, sizeof err));
    EXPECT_TRUE(strstr(err, "two platforms") != NULL);
}

DESCRIBE(a_platform_with_an_unknown_format_is_refused) {
    char err[512] = "";
    EXPECT_FALSE(read_with("format = \"pacman\"", "format = \"snap\"", err, sizeof err));
    EXPECT_TRUE(strstr(err, "format") != NULL);
}

/* An older molto must refuse the form rather than misread it, which only
   happens if the recipe says it needs a newer reader. */
DESCRIBE(a_platform_recipe_must_declare_schema_three) {
    char text[8192];
    snprintf(text, sizeof text, "schema = 2%s", strstr(RECIPE, "\nform"));
    char parse_err[256] = "";
    toml_document *doc = toml_parse(text, parse_err, sizeof parse_err);
    ASSERT_TRUE(doc != NULL);
    recipe_coordinate coordinate;
    char err[512] = "";
    EXPECT_FALSE(recipe_read_coordinate(doc_from_toml(doc), &coordinate, err, sizeof err));
    EXPECT_TRUE(strstr(err, "schema 3") != NULL);
    toml_free(doc);

    doc = toml_parse(RECIPE, parse_err, sizeof parse_err);
    ASSERT_TRUE(doc != NULL);
    EXPECT_TRUE(recipe_read_coordinate(doc_from_toml(doc), &coordinate, err, sizeof err));
    EXPECT_EQ(recipe_form_platform, coordinate.form);
    toml_free(doc);
}

/* The lock records one digest for the package; it has to be the same whichever
   platform a machine chooses, and change the moment any pin does. */
DESCRIBE(a_platform_recipe_digest_covers_every_pin) {
    platform_recipe recipe;
    char err[512] = "";
    ASSERT_TRUE(read_recipe(RECIPE, &recipe, err, sizeof err));
    char first[65];
    char again[65];
    platform_recipe_digest(&recipe, first);
    platform_recipe_digest(&recipe, again);
    EXPECT_STREQ(first, again);
    platform_recipe_free(&recipe);

    char text[8192];
    const char *pin = strstr(RECIPE, "sha256 = \"" DIGEST_B);
    ASSERT_TRUE(pin != NULL);
    snprintf(text, sizeof text, "%.*ssha256 = \"" DIGEST_A "%s", (int)(pin - RECIPE), RECIPE,
             pin + strlen("sha256 = \"" DIGEST_B));
    ASSERT_TRUE(read_recipe(text, &recipe, err, sizeof err));
    char changed[65];
    platform_recipe_digest(&recipe, changed);
    EXPECT_TRUE(strcmp(first, changed) != 0);
    platform_recipe_free(&recipe);
}

/* --- choosing --- */

DESCRIBE(os_release_gives_the_distribution_and_its_version) {
    platform_host host = {0};
    platform_host_read_os_release("NAME=\"Ubuntu\"\nVERSION_ID=\"24.04\"\nID=ubuntu\n"
                                  "ID_LIKE=debian\n",
                                  &host);
    EXPECT_STREQ("ubuntu", host.distro);
    EXPECT_STREQ("24.04", host.distro_version);
}

DESCRIBE(a_machine_gets_the_first_platform_it_matches) {
    platform_recipe recipe;
    char err[512] = "";
    ASSERT_TRUE(read_recipe(RECIPE, &recipe, err, sizeof err));

    platform_host ubuntu = {.os = "linux", .arch = "x86_64", .distro = "ubuntu",
                            .distro_version = "24.04"};
    size_t index = 99;
    EXPECT_TRUE(platform_choose(&recipe, &ubuntu, &index, err, sizeof err));
    EXPECT_EQ(0u, index);

    /* A rolling distribution names no version, so any of its releases match. */
    platform_host arch = {.os = "linux", .arch = "x86_64", .distro = "arch"};
    EXPECT_TRUE(platform_choose(&recipe, &arch, &index, err, sizeof err));
    EXPECT_EQ(1u, index);

    platform_host mac = {.os = "macos", .arch = "aarch64", .os_version = "15.4"};
    EXPECT_TRUE(platform_choose(&recipe, &mac, &index, err, sizeof err));
    EXPECT_EQ(2u, index);

    platform_recipe_free(&recipe);
}

DESCRIBE(a_machine_no_platform_serves_is_told_which_ones_are_served) {
    platform_recipe recipe;
    char err[512] = "";
    ASSERT_TRUE(read_recipe(RECIPE, &recipe, err, sizeof err));

    platform_host old_ubuntu = {.os = "linux", .arch = "x86_64", .distro = "ubuntu",
                                .distro_version = "22.04"};
    size_t index = 0;
    EXPECT_FALSE(platform_choose(&recipe, &old_ubuntu, &index, err, sizeof err));
    EXPECT_TRUE(strstr(err, "ubuntu 22.04") != NULL);
    EXPECT_TRUE(strstr(err, "ubuntu-24.04-x86_64, arch-x86_64, macos-arm64") != NULL);

    /* Older than the bottles were built for. */
    platform_host old_mac = {.os = "macos", .arch = "aarch64", .os_version = "14.6"};
    EXPECT_FALSE(platform_choose(&recipe, &old_mac, &index, err, sizeof err));

    platform_recipe_free(&recipe);
}

DESCRIBE(versions_compare_number_by_number) {
    EXPECT_EQ(-1, platform_version_compare("14.6", "15.0"));
    EXPECT_EQ(0, platform_version_compare("15", "15.0"));
    EXPECT_EQ(1, platform_version_compare("15.10", "15.9"));
    EXPECT_EQ(1, platform_version_compare("26", "15.4"));
}

/* --- ar: a .deb --- */

static size_t ar_put(unsigned char *out, size_t at, const char *name, const char *body) {
    const size_t length = strlen(body);
    char header[61];
    snprintf(header, sizeof header, "%-16s%-12s%-6s%-6s%-8s%-10zu`\n", name, "0", "0", "0", "644",
             length);
    memcpy(out + at, header, 60);
    memcpy(out + at + 60, body, length);
    at += 60 + length;
    if(length & 1u)
        out[at++] = '\n';
    return at;
}

DESCRIBE(a_deb_gives_up_its_data_member) {
    unsigned char deb[512];
    memcpy(deb, "!<arch>\n", 8);
    size_t at = ar_put(deb, 8, "debian-binary", "2.0\n");
    at = ar_put(deb, at, "control.tar.zst", "abc");
    const size_t data_header = at;
    at = ar_put(deb, at, "data.tar.xz", "hello");

    size_t offset = 0;
    size_t length = 0;
    ASSERT_TRUE(platform_ar_member(deb, at, "data.tar", &offset, &length));
    EXPECT_EQ(data_header + 60, offset);
    EXPECT_EQ(5u, length);
    EXPECT_TRUE(memcmp(deb + offset, "hello", 5) == 0);

    EXPECT_FALSE(platform_ar_member(deb, at, "nothing", &offset, &length));
    /* A member claiming more bytes than the archive holds is not followed. */
    EXPECT_FALSE(platform_ar_member(deb, data_header + 62, "data.tar", &offset, &length));
}

/* --- rpm --- */

static void put_be32(unsigned char *at, uint32_t value) {
    at[0] = (unsigned char)(value >> 24);
    at[1] = (unsigned char)(value >> 16);
    at[2] = (unsigned char)(value >> 8);
    at[3] = (unsigned char)value;
}

static size_t rpm_header(unsigned char *out, size_t at, uint32_t entries, uint32_t store) {
    static const unsigned char MAGIC[] = {0x8e, 0xad, 0xe8, 0x01};
    memcpy(out + at, MAGIC, sizeof MAGIC);
    memset(out + at + 4, 0, 4);
    put_be32(out + at + 8, entries);
    put_be32(out + at + 12, store);
    const size_t length = 16 + (size_t)entries * 16 + store;
    memset(out + at + 16, 0, length - 16);
    return at + length;
}

DESCRIBE(an_rpm_payload_starts_after_both_headers) {
    unsigned char rpm[256] = {0xed, 0xab, 0xee, 0xdb};
    /* The signature header is 37 bytes, so it is padded to 40. */
    size_t at = rpm_header(rpm, 96, 1, 5);
    ASSERT_EQ(96u + 37u, at);
    at = rpm_header(rpm, 96 + 40, 0, 3);
    memcpy(rpm + at, "PAYLOAD", 7);

    size_t offset = 0;
    ASSERT_TRUE(platform_rpm_payload(rpm, at + 7, &offset));
    EXPECT_EQ(at, offset);
    EXPECT_TRUE(memcmp(rpm + offset, "PAYLOAD", 7) == 0);

    rpm[0] = 0;
    EXPECT_FALSE(platform_rpm_payload(rpm, at + 7, &offset));
}

/* --- cpio --- */

static size_t cpio_put(unsigned char *out, size_t at, const char *name, unsigned mode,
                       const char *body) {
    const size_t name_size = strlen(name) + 1;
    const size_t body_size = strlen(body);
    char header[111];
    snprintf(header, sizeof header,
             "070701%08X%08X%08X%08X%08X%08X%08zX%08X%08X%08X%08X%08zX%08X", 0u, mode, 0u, 0u, 1u,
             0u, body_size, 0u, 0u, 0u, 0u, name_size, 0u);
    memcpy(out + at, header, 110);
    memcpy(out + at + 110, name, name_size);
    at += 110 + name_size;
    while(at % 4 != 0)
        out[at++] = 0;
    memcpy(out + at, body, body_size);
    at += body_size;
    while(at % 4 != 0)
        out[at++] = 0;
    return at;
}

DESCRIBE(a_cpio_archive_unpacks_its_files_directories_and_links) {
    char dir[256];
    ASSERT_TRUE(moltest_temp_dir("molto_cpio", dir, sizeof dir));
    unsigned char archive[2048];
    size_t at = cpio_put(archive, 0, "./usr/include/gtk-4.0", 0040755, "");
    at = cpio_put(archive, at, "./usr/include/gtk-4.0/gtk.h", 0100644, "#pragma once\n");
    at = cpio_put(archive, at, "./usr/lib64/libgtk-4.so", 0120777, "libgtk-4.so.1");
    at = cpio_put(archive, at, "TRAILER!!!", 0, "");

    char err[512] = "";
    ASSERT_TRUE(platform_cpio_unpack(archive, at, dir, err, sizeof err));

    char path[512];
    snprintf(path, sizeof path, "%s/usr/include/gtk-4.0/gtk.h", dir);
    char *text = fs_read_file(path);
    ASSERT_TRUE(text != NULL);
    EXPECT_STREQ("#pragma once\n", text);
    free(text);

    char target[256] = "";
    snprintf(path, sizeof path, "%s/usr/lib64/libgtk-4.so", dir);
    ASSERT_TRUE(fs_link_target(path, target, sizeof target));
    EXPECT_STREQ("libgtk-4.so.1", target);
    (void)fs_remove_tree(dir);
}

/* A symlink unpacked first can point anywhere; writing a later member through
   it is how an archive escapes the directory it is unpacked into. */
DESCRIBE(a_cpio_member_written_through_a_link_that_leaves_is_refused) {
    char dir[256];
    char outside[256];
    ASSERT_TRUE(moltest_temp_dir("molto_cpio", dir, sizeof dir));
    ASSERT_TRUE(moltest_temp_dir("molto_cpio_out", outside, sizeof outside));
    unsigned char archive[2048];
    size_t at = cpio_put(archive, 0, "./usr/lib", 0120777, outside);
    at = cpio_put(archive, at, "./usr/lib/planted", 0100644, "x");
    at = cpio_put(archive, at, "TRAILER!!!", 0, "");

    char err[512] = "";
    EXPECT_FALSE(platform_cpio_unpack(archive, at, dir, err, sizeof err));
    EXPECT_TRUE(strstr(err, "outside") != NULL);

    char planted[512];
    snprintf(planted, sizeof planted, "%s/planted", outside);
    EXPECT_FALSE(fs_path_exists(planted));
    (void)fs_remove_tree(dir);
    (void)fs_remove_tree(outside);
}

DESCRIBE(a_cpio_member_naming_a_parent_directory_is_refused) {
    char dir[256];
    ASSERT_TRUE(moltest_temp_dir("molto_cpio", dir, sizeof dir));
    unsigned char archive[1024];
    size_t at = cpio_put(archive, 0, "./usr/../../escaped", 0100644, "x");
    at = cpio_put(archive, at, "TRAILER!!!", 0, "");
    char err[512] = "";
    EXPECT_FALSE(platform_cpio_unpack(archive, at, dir, err, sizeof err));
    EXPECT_TRUE(strstr(err, "climbs out") != NULL);
    (void)fs_remove_tree(dir);
}

/* --- Mach-O --- */

static void put_le32(unsigned char *at, uint32_t value) {
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)(value >> 8);
    at[2] = (unsigned char)(value >> 16);
    at[3] = (unsigned char)(value >> 24);
}

/* One dylib command naming `name`, padded to eight bytes as ld pads them. */
static size_t dylib_command(unsigned char *out, size_t at, uint32_t command, const char *name) {
    const size_t length = (24 + strlen(name) + 1 + 7) & ~(size_t)7;
    memset(out + at, 0, length);
    put_le32(out + at, command);
    put_le32(out + at + 4, (uint32_t)length);
    put_le32(out + at + 8, 24);
    memcpy(out + at + 24, name, strlen(name));
    return at + length;
}

DESCRIBE(a_bottle_s_install_names_are_relocated_to_the_rpath) {
    unsigned char macho[512] = {0};
    put_le32(macho, 0xfeedfacfu);
    size_t at = 32;
    at = dylib_command(macho, at, 0x0du, "@@HOMEBREW_PREFIX@@/opt/glib/lib/libglib-2.0.0.dylib");
    at = dylib_command(macho, at, 0x0cu, "@@HOMEBREW_CELLAR@@/pcre2/10.49/lib/libpcre2-8.0.dylib");
    at = dylib_command(macho, at, 0x0cu, "/usr/lib/libSystem.B.dylib");
    put_le32(macho + 16, 3);
    put_le32(macho + 20, (uint32_t)(at - 32));

    bool changed = false;
    ASSERT_TRUE(platform_macho_relocate(macho, sizeof macho, &changed));
    EXPECT_TRUE(changed);
    EXPECT_STREQ("@rpath/libglib-2.0.0.dylib", (const char *)macho + 32 + 24);
    const size_t second = 32 + ((24 + 52 + 1 + 7) & ~(size_t)7);
    EXPECT_STREQ("@rpath/libpcre2-8.0.dylib", (const char *)macho + second + 24);
    /* What the system provides is left where it is. */
    const size_t third = second + ((24 + 54 + 1 + 7) & ~(size_t)7);
    EXPECT_STREQ("/usr/lib/libSystem.B.dylib", (const char *)macho + third + 24);

    /* Already relocated: nothing changes, so nothing needs signing again. */
    ASSERT_TRUE(platform_macho_relocate(macho, sizeof macho, &changed));
    EXPECT_FALSE(changed);
}

DESCRIBE(a_mach_o_whose_commands_run_past_the_file_is_refused) {
    unsigned char macho[64] = {0};
    put_le32(macho, 0xfeedfacfu);
    put_le32(macho + 16, 1);
    put_le32(macho + 20, 4096);
    bool changed = false;
    EXPECT_FALSE(platform_macho_relocate(macho, sizeof macho, &changed));
    put_le32(macho, 0xcafebabeu); /* universal: not one this handles */
    EXPECT_FALSE(platform_macho_relocate(macho, sizeof macho, &changed));
}

/* --- beside an executable --- */

DESCRIBE(a_bundled_runtime_s_dlls_are_copied_beside_the_executable) {
    char bin[256];
    char out[256];
    ASSERT_TRUE(moltest_temp_dir("molto_bin", bin, sizeof bin));
    ASSERT_TRUE(moltest_temp_dir("molto_exe", out, sizeof out));
    char path[512];
    snprintf(path, sizeof path, "%s/libgtk-4-1.dll", bin);
    ASSERT_TRUE(fs_write_file(path, "dll"));
    snprintf(path, sizeof path, "%s/gtk4-demo.exe", bin);
    ASSERT_TRUE(fs_write_file(path, "exe"));

    str_list dirs;
    str_list_init(&dirs);
    ASSERT_TRUE(str_list_push(&dirs, bin));
    char err[512] = "";
    EXPECT_TRUE(platform_copy_runtime(&dirs, out, err, sizeof err));

    snprintf(path, sizeof path, "%s/libgtk-4-1.dll", out);
    EXPECT_TRUE(fs_path_exists(path));
    snprintf(path, sizeof path, "%s/gtk4-demo.exe", out);
    EXPECT_FALSE(fs_path_exists(path));

    str_list_free(&dirs);
    (void)fs_remove_tree(bin);
    (void)fs_remove_tree(out);
}
