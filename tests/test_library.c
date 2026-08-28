#include <moltest.h>

#include <molto/build/library.h>

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
    (void)library_names_of(kind, package, version, &names, err, sizeof err);
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

/* A guess would put the wrong number in a soname, which is the one place a
   wrong number is a promise about ABI. */
MOLTEST(a_version_that_is_not_semver_cannot_name_a_shared_library) {
    library_names names;
    char err[256] = "";
    EXPECT_FALSE(library_names_of(artifact_shared, "calculator", "nightly", &names, err,
                                  sizeof err));
    EXPECT_NOT_NULL(strstr(err, "semver"));
}

/* The same version names a static library perfectly well, because nothing about
   a `.a` depends on it. */
MOLTEST(a_static_library_needs_no_version_at_all) {
    library_names names;
    char err[256] = "";
    EXPECT_TRUE(library_names_of(artifact_static, "calculator", "nightly", &names, err,
                                 sizeof err));
    EXPECT_STREQ("libcalculator.a", names.file);
}

MOLTEST(source_is_a_recipe_s_business_and_not_something_to_build) {
    library_names names;
    char err[256] = "";
    EXPECT_FALSE(library_names_of(artifact_source, "calculator", "1.0.0", &names, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "registry"));
}

MOLTEST(a_package_with_no_name_cannot_be_built) {
    library_names names;
    char err[256] = "";
    EXPECT_FALSE(library_names_of(artifact_static, "", "1.0.0", &names, err, sizeof err));
}
