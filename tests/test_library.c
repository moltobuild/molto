#include <moltest.h>

#include <molto/build/library.h>
#include <molto/services/fs_service.h>

#include <string.h>

/*
 * What a built thing is called.
 *
 * These names are not molto's to choose. `libz.so.1.2.13` with a soname of
 * `libz.so.1` is what every linker and every packager on a Unix expects, so
 * what is under test is conformance to a convention rather than a preference —
 * which is why the expected strings are written out in full rather than
 * composed by the test.
 */

static library_names named(artifact_kind kind, const char *package, const char *version) {
    library_names names;
    char err[256] = "";
    (void)library_names_of(kind, package, version, "x86_64-unknown-linux-gnu", &names, err,
                           sizeof err);
    return names;
}

/* The same, for a platform that is not the one running. Naming is a parameter
   and not a compile-time test precisely so that this is possible: every
   platform's answer is checkable from every platform, and none of these cases
   waits for a runner. */
static library_names named_for(const char *platform, artifact_kind kind, const char *package,
                               const char *version) {
    library_names names;
    char err[256] = "";
    (void)library_names_of(kind, package, version, platform, &names, err, sizeof err);
    return names;
}

MOLTEST(an_executable_is_called_what_the_package_is_called) {
    const library_names names = named(artifact_executable, "calculator", "0.1.0");
    EXPECT_STREQ("calculator", names.file);

    /* Nothing to record and nothing to point at it: the two fields exist for
       the one kind that has them. */
    EXPECT_STREQ("", names.soname);
    EXPECT_STREQ("", names.devlink);
}

/*
 * And the same name on every platform, Windows included.
 *
 * These names reach the IR, where `artifact.path` says what a project builds
 * and has to read the same everywhere. Appending the host's `.exe` here is
 * what a first attempt did, and `frontend_native_describes_a_manifest_and_its_
 * sources` caught it: the document came back saying `app.exe`, which is a
 * machine's answer written into a portable description.
 *
 * The suffix is real and still needed -- it goes on in `build_service`, where
 * the path on disk is composed and a filename is what is wanted.
 */
MOLTEST(an_executables_name_carries_no_platform_of_its_own) {
    const library_names names = named(artifact_executable, "calculator", "0.1.0");
    EXPECT_STREQ("calculator", names.file);
    EXPECT_EQ(NULL, strstr(names.file, ".exe"));
}

MOLTEST(a_static_library_takes_the_lib_prefix_and_the_a_suffix) {
    const library_names names = named(artifact_static, "calculator", "0.1.0");
    EXPECT_STREQ("libcalculator.a", names.file);
    EXPECT_STREQ("", names.soname);
}

MOLTEST(a_shared_library_carries_its_whole_version_in_the_file) {
    const library_names names = named(artifact_shared, "calculator", "1.2.3");
    EXPECT_STREQ("libcalculator.so.1.2.3", names.file);
}

/* The soname carries the major and nothing else, which is the whole convention:
   a program linked against `libcalculator.so.1` keeps running when 1.2.3 is
   replaced by 1.9.0, and stops when it is replaced by 2.0.0. */
MOLTEST(a_soname_carries_only_the_major) {
    const library_names names = named(artifact_shared, "calculator", "1.2.3");
    EXPECT_STREQ("libcalculator.so.1", names.soname);

    const library_names later = named(artifact_shared, "calculator", "1.9.0");
    EXPECT_STREQ("libcalculator.so.1", later.soname);

    const library_names breaking = named(artifact_shared, "calculator", "2.0.0");
    EXPECT_STREQ("libcalculator.so.2", breaking.soname);
}

MOLTEST(the_unversioned_name_is_what_a_link_resolves_through) {
    const library_names names = named(artifact_shared, "calculator", "1.2.3");
    EXPECT_STREQ("libcalculator.so", names.devlink);
}

MOLTEST(a_zero_major_is_a_soname_like_any_other) {
    /* The version `molto new` writes. It has to produce a loadable library or
       every project's first shared build is broken. */
    const library_names names = named(artifact_shared, "calculator", "0.1.0");
    EXPECT_STREQ("libcalculator.so.0.1.0", names.file);
    EXPECT_STREQ("libcalculator.so.0", names.soname);
}

/*
 * macOS spells the same three facts differently, and not by a suffix.
 *
 * `libfoo.so.1.2.3` becomes `libfoo.1.2.3.dylib`: the version moved from after
 * the extension to before it, so there is no way to write one of these names in
 * terms of the other. A first attempt that swapped `.so` for `.dylib` would
 * produce `libcalculator.dylib.0.1.0`, which nothing loads and which reads
 * plausible enough to survive a review.
 */
MOLTEST(a_shared_library_on_darwin_is_a_dylib_with_the_version_in_the_middle) {
    const library_names names = named_for("aarch64-darwin", artifact_shared, "calculator", "0.1.0");
    EXPECT_STREQ("libcalculator.0.1.0.dylib", names.file);
    EXPECT_STREQ("libcalculator.0.dylib", names.soname);
    EXPECT_STREQ("libcalculator.dylib", names.devlink);
}

/*
 * And the option that records it, which is the half a compiler will not warn
 * about.
 *
 * `-soname` is GNU ld's. Apple's ld64 does not know it and says so —
 * `unknown options: -soname` — after the compile has already succeeded, which
 * is why this belongs to the platform and not to the document: `frontend_native`
 * is handed a root and a profile and is never told which platform this is
 * (RFC-0018).
 */
MOLTEST(the_name_a_library_records_is_spelled_by_its_linker) {
    const library_names gnu =
        named_for("x86_64-unknown-linux-gnu", artifact_shared, "calculator", "0.1.0");
    EXPECT_STREQ("-Wl,-soname,libcalculator.so.0", gnu.name_option);

    const library_names apple = named_for("aarch64-darwin", artifact_shared, "calculator", "0.1.0");
    EXPECT_STREQ("-Wl,-install_name,libcalculator.0.dylib", apple.name_option);

    /* A bare filename on both. A path here would write this build's directory
       inside an artifact whose whole purpose is to be copied elsewhere. */
    EXPECT_NULL(strchr(apple.name_option + strlen("-Wl,-install_name,"), '/'));
}

/* Every spelling a person might reach for at `--target`, and the one the
   registry actually uses. A triple that is not any of them is not a Mac. */
MOLTEST(darwin_is_recognised_by_the_names_people_write) {
    static const char *const macs[] = {"aarch64-darwin", "x86_64-apple-darwin", "arm64-macos"};
    for(size_t i = 0; i < sizeof macs / sizeof macs[0]; i++) {
        const library_names names = named_for(macs[i], artifact_shared, "calculator", "0.1.0");
        EXPECT_STREQ("libcalculator.0.1.0.dylib", names.file);
    }
    const library_names other =
        named_for("x86_64-unknown-linux-gnu", artifact_shared, "calculator", "0.1.0");
    EXPECT_STREQ("libcalculator.so.0.1.0", other.file);
}

/* The kinds that carry no recorded name carry no option either, on any
   platform: an executable has nothing to record and a `.a` is not loaded. */
MOLTEST(only_a_shared_library_records_a_name) {
    EXPECT_STREQ("", named_for("aarch64-darwin", artifact_executable, "calculator", "0.1.0")
                         .name_option);
    EXPECT_STREQ("", named_for("aarch64-darwin", artifact_static, "calculator", "0.1.0")
                         .name_option);
}

/* A guess would put the wrong number in a soname, which is the one place a
   wrong number is a promise about ABI. */
MOLTEST(a_version_that_is_not_semver_cannot_name_a_shared_library) {
    library_names names;
    char err[256] = "";
    EXPECT_FALSE(library_names_of(artifact_shared, "calculator", "nightly", NULL, &names, err,
                                  sizeof err));
    EXPECT_NOT_NULL(strstr(err, "semver"));
}

/* The same version names a static library perfectly well, because nothing about
   a `.a` depends on it. */
MOLTEST(a_static_library_needs_no_version_at_all) {
    library_names names;
    char err[256] = "";
    EXPECT_TRUE(library_names_of(artifact_static, "calculator", "nightly", NULL, &names, err,
                                 sizeof err));
    EXPECT_STREQ("libcalculator.a", names.file);
}

MOLTEST(source_is_a_recipe_s_business_and_not_something_to_build) {
    library_names names;
    char err[256] = "";
    EXPECT_FALSE(library_names_of(artifact_source, "calculator", "1.0.0", NULL, &names, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "registry"));
}

MOLTEST(a_package_with_no_name_cannot_be_built) {
    library_names names;
    char err[256] = "";
    EXPECT_FALSE(library_names_of(artifact_static, "", "1.0.0", NULL, &names, err, sizeof err));
}
