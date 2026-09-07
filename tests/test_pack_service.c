#include <moltest.h>

#include <molto/services/fs_service.h>
#include <molto/services/pack_service.h>

#include <stdio.h>
#include <string.h>

/*
 * Packing a directory into the artifact a coordinate is published as.
 *
 * The choice of compressor is the part worth pinning: it is not a preference
 * but a constraint, and getting it wrong produces an archive that uploads,
 * publishes and then cannot be opened on the machine it was built for.
 */

MOLTEST(a_windows_target_packs_as_gzip) {
    /* Not a preference. The tar.exe Windows ships is bsdtar with libarchive
       linked against zlib alone, so gzip is the only packing pickup can open
       there -- which is what a toolchain running on Windows is packed for. */
    EXPECT_STREQ("tar.gz", pack_default_format("windows-x86_64"));
    EXPECT_STREQ("tar.gz", pack_default_format("windows-aarch64"));
}

MOLTEST(every_other_target_packs_as_zstd) {
    EXPECT_STREQ("tar.zst", pack_default_format("linux-x86_64"));
    EXPECT_STREQ("tar.zst", pack_default_format("darwin-aarch64"));
    EXPECT_STREQ("tar.zst", pack_default_format("any"));
}

/* A target nobody named is not a Windows one, and guessing gzip for it would
   cost every other platform half its compression. */
MOLTEST(an_absent_target_is_not_a_windows_one) {
    EXPECT_STREQ("tar.zst", pack_default_format(NULL));
    EXPECT_STREQ("tar.zst", pack_default_format(""));
}

/* `windows` is not `windows-`: a target has to name an architecture, and a
   prefix match without the separator would take `windowsomething` with it. */
MOLTEST(the_windows_test_is_on_the_whole_prefix) {
    EXPECT_STREQ("tar.zst", pack_default_format("windows"));
}

MOLTEST(only_two_packings_are_known) {
    EXPECT_TRUE(pack_format_is_known("tar.zst"));
    EXPECT_TRUE(pack_format_is_known("tar.gz"));

    /* tar.xz is the one that has to stay out. It was tried, and the tar
       Windows ships hands xz to a program that is not there -- and when it is,
       fills its stdin while waiting on its stdout and deadlocks. */
    EXPECT_FALSE(pack_format_is_known("tar.xz"));
    EXPECT_FALSE(pack_format_is_known("zip"));
    EXPECT_FALSE(pack_format_is_known(""));
    EXPECT_FALSE(pack_format_is_known(NULL));
}

MOLTEST(an_archive_is_named_for_its_coordinate) {
    char name[128] = "";
    EXPECT_TRUE(pack_archive_name("llvm-mingw", "23.1.0", "windows-x86_64", "tar.gz", name,
                                  sizeof name));
    EXPECT_STREQ("llvm-mingw-23.1.0-windows-x86_64.tar.gz", name);
}

/* Refused rather than truncated: a clipped name is a name pickup will not
   find, and finding that out after the upload is the expensive way. */
MOLTEST(a_name_that_would_not_fit_is_refused) {
    char name[8] = "";
    EXPECT_FALSE(pack_archive_name("llvm-mingw", "23.1.0", "windows-x86_64", "tar.gz", name,
                                   sizeof name));
}

MOLTEST(packing_refuses_a_format_it_does_not_write) {
    char dir[512];
    ASSERT_TRUE(moltest_temp_dir("molto_pack", dir, sizeof dir));

    char archive[600];
    snprintf(archive, sizeof archive, "%s/out.tar.xz", dir);

    char err[256] = "";
    EXPECT_FALSE(pack_directory(dir, archive, "tar.xz", err, sizeof err));
    EXPECT_TRUE(strlen(err) > 0);
}

MOLTEST(packing_refuses_something_that_is_not_a_directory) {
    char dir[512];
    ASSERT_TRUE(moltest_temp_dir("molto_pack", dir, sizeof dir));

    char file[600];
    snprintf(file, sizeof file, "%s/a_file", dir);
    FILE *handle = fopen(file, "wb");
    ASSERT_NOT_NULL(handle);
    (void)fclose(handle);

    char archive[600];
    snprintf(archive, sizeof archive, "%s/out.tar.gz", dir);

    char err[256] = "";
    EXPECT_FALSE(pack_directory(file, archive, "tar.gz", err, sizeof err));
    EXPECT_TRUE(strlen(err) > 0);
}

/*
 * The whole thing, against whichever tar this machine will actually run.
 *
 * gzip because that is the one packing both implementations can write: GNU tar
 * delegates to the program, bsdtar has zlib compiled in. A machine whose tar
 * can do neither is a machine that cannot publish, and the test says so with
 * the message rather than failing silently.
 */
MOLTEST(packing_writes_an_archive_of_the_tree) {
    char dir[512];
    ASSERT_TRUE(moltest_temp_dir("molto_pack", dir, sizeof dir));

    char tree[600];
    snprintf(tree, sizeof tree, "%s/tree/bin", dir);
    ASSERT_TRUE(fs_make_dirs(tree));

    char binary[800];
    snprintf(binary, sizeof binary, "%s/a_driver", tree);
    FILE *handle = fopen(binary, "wb");
    ASSERT_NOT_NULL(handle);
    fputs("not a compiler", handle);
    ASSERT_EQ(0, fclose(handle));

    char source[600];
    snprintf(source, sizeof source, "%s/tree", dir);

    char archive[700];
    snprintf(archive, sizeof archive, "%s/packed.tar.gz", dir);

    char err[256] = "";
    const bool packed = pack_directory(source, archive, "tar.gz", err, sizeof err);
    if (!packed)
        fprintf(stderr, "pack_directory: %s\n", err);

    /* Asserted rather than tolerated: every tar worth the name writes gzip —
       GNU tar hands the stream to a program that is everywhere, and bsdtar has
       zlib compiled in. A machine where this fails cannot publish anything,
       and the message above says which half is missing. */
    ASSERT_TRUE(packed);
    EXPECT_TRUE(fs_path_exists(archive));

    (void)fs_remove_tree(dir);
}
