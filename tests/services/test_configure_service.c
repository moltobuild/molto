#include <moltest.h>

#include <molto/services/configure_service.h>
#include <molto/services/fs_service.h>
#include <molto/services/process_service.h>
#include <molto/services/tool_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Running a dependency's configure, against a fake one.
 *
 * The fake does what a real one does to the tree — writes a header — and
 * counts its own runs, so the stamp's job (running once per compiler) can be
 * measured. It needs `sh` and `make`, which is what a real configure needs. */

#define PATH_MAX_LEN 512

typedef struct {
    char root[64];
} sandbox;

static bool make_source(sandbox *at) {
    if (!moltest_temp_dir("molto_configure", at->root, sizeof at->root))
        return false;
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/configure", at->root);
    if (!fs_write_file(file, "echo run >> runs.txt\n"
                             "echo \"#define CONFIGURED_FOR \\\"$CC $*\\\"\" > config.h\n"
                             "echo \"#define TOOL \\\"$TOOL\\\"\" >> config.h\n"))
        return false;
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof dir, "%s/src/port", at->root);
    if (!fs_make_dirs(dir))
        return false;
    snprintf(file, sizeof file, "%s/src/port/Makefile", at->root);
    return fs_write_file(file, "paths.h:\n\techo '#define PATHS 1' > paths.h\n");
}

static int runs(const sandbox *at) {
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/runs.txt", at->root);
    char *text = fs_read_file(file);
    int count = 0;
    for (const char *c = text; c != NULL && *c != '\0'; c++)
        count += *c == '\n';
    free(text);
    return count;
}

static recipe_build delegated(void) {
    recipe_build build = {0};
    build.system = recipe_build_autotools;
    build.via = recipe_via_delegate;
    snprintf(build.args[build.arg_count++], RECIPE_BUILD_ARG_MAX, "--without-icu");
    snprintf(build.env[build.env_count++], RECIPE_BUILD_ARG_MAX, "TOOL=true");
    snprintf(build.targets[build.target_count++], RECIPE_BUILD_ARG_MAX, "src/port/paths.h");
    return build;
}

DESCRIBE(configure_writes_what_upstream_configure_writes) {
    sandbox at;
    ASSERT_TRUE(make_source(&at));
    const recipe_build build = delegated();
    char err[512] = "";
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "cc", NULL, NULL, err, sizeof err));

    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/config.h", at.root);
    char *text = fs_read_file(file);
    ASSERT_NOT_NULL(text);
    /* CC is the compiler molto resolved, the arguments are the recipe's, and
       so is the environment. */
    EXPECT_NOT_NULL(strstr(text, "cc --without-icu"));
    EXPECT_NOT_NULL(strstr(text, "#define TOOL \"true\""));
    free(text);
    snprintf(file, sizeof file, "%s/src/port/paths.h", at.root);
    EXPECT_TRUE(fs_path_exists(file));
    (void)fs_remove_tree(at.root);
}

DESCRIBE(configure_runs_once_per_compiler) {
    sandbox at;
    ASSERT_TRUE(make_source(&at));
    const recipe_build build = delegated();
    char err[512] = "";
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "cc", NULL, NULL, err, sizeof err));
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "cc", NULL, NULL, err, sizeof err));
    EXPECT_EQ(1, runs(&at));

    /* Another compiler is another machine as far as configure is concerned. */
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "gcc", NULL, NULL, err, sizeof err));
    EXPECT_EQ(2, runs(&at));
    (void)fs_remove_tree(at.root);
}

DESCRIBE(a_failing_configure_is_reported_with_its_log) {
    sandbox at;
    ASSERT_TRUE(make_source(&at));
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/configure", at.root);
    ASSERT_TRUE(fs_write_file(file, "echo 'configure: error: no compiler' >&2\nexit 1\n"));
    const recipe_build build = delegated();
    char err[512] = "";
    EXPECT_FALSE(configure_dependency("fake", at.root, &build, "cc", NULL, NULL, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "fake"));
    EXPECT_NOT_NULL(strstr(err, "config.log"));
    (void)fs_remove_tree(at.root);
}

DESCRIBE(the_fingerprint_changes_with_what_shapes_the_answer) {
    recipe_build build = delegated();
    char first[65];
    char second[65];
    configure_fingerprint(&build, "cc", NULL, NULL, first);
    configure_fingerprint(&build, "cc", NULL, NULL, second);
    EXPECT_STREQ(first, second);
    snprintf(build.args[0], RECIPE_BUILD_ARG_MAX, "--with-icu");
    configure_fingerprint(&build, "cc", NULL, NULL, second);
    EXPECT_TRUE(strcmp(first, second) != 0);
    configure_fingerprint(&build, "cc", "x86_64-w64-mingw32", NULL, first);
    EXPECT_TRUE(strcmp(first, second) != 0);
}

DESCRIBE(a_build_molto_does_not_configure_runs_nothing) {
    const recipe_build none = {0};
    char err[512] = "";
    EXPECT_TRUE(configure_dependency("fake", "/nonexistent", &none, "cc", NULL, NULL, err, sizeof err));
}

/* --- what the configured build compiles (RFC-0025) --- */

/* A configure that takes its compiler as an argument, as FFmpeg's does, and a
   Makefile that compiles one file and archives it. */
static bool make_buildable(sandbox *at) {
    if (!make_source(at))
        return false;
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/configure", at->root);
    if (!fs_write_file(file, "echo run >> runs.txt\necho \"$*\" > args.txt\n"))
        return false;
    snprintf(file, sizeof file, "%s/a.c", at->root);
    if (!fs_write_file(file, "int a(void) { return 1; }\n"))
        return false;
    snprintf(file, sizeof file, "%s/Makefile", at->root);
    return fs_write_file(file, "liba.a: a.o\n\tar rc liba.a a.o\n"
                               "a.o: a.c\n\tcc -Iinc -DA=1 -MMD -c -o a.o a.c\n");
}

DESCRIBE(the_compiler_reaches_configure_as_an_argument) {
    sandbox at;
    ASSERT_TRUE(make_buildable(&at));
    recipe_build build = delegated();
    build.target_count = 0;
    snprintf(build.args[0], RECIPE_BUILD_ARG_MAX, "--cc={cc}");
    char err[512] = "";
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "/opt/cc", NULL, NULL, err,
                                     sizeof err));
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/args.txt", at.root);
    char *text = fs_read_file(file);
    ASSERT_NOT_NULL(text);
    EXPECT_NOT_NULL(strstr(text, "--cc=/opt/cc"));
    free(text);
    (void)fs_remove_tree(at.root);
}

DESCRIBE(make_says_what_it_compiles_and_the_answer_is_kept) {
    sandbox at;
    ASSERT_TRUE(make_buildable(&at));
    recipe_build build = delegated();
    build.target_count = 0;
    build.sources = recipe_sources_make;
    char err[512] = "";
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "cc", NULL, NULL, err, sizeof err));

    compile_lines lines;
    compile_lines_init(&lines);
    ASSERT_TRUE(configure_compile_lines("fake", at.root, &build, "cc", NULL, &lines, err, sizeof err));
    ASSERT_EQ(1, (int)lines.count);
    EXPECT_STREQ("a.c", lines.lines[0].source);
    compile_lines_free(&lines);

    /* Read back from beside the stamp: a Makefile that no longer answers is
       not asked. */
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/Makefile", at.root);
    ASSERT_TRUE(fs_write_file(file, "liba.a:\n\tfalse\n"));
    compile_lines_init(&lines);
    ASSERT_TRUE(configure_compile_lines("fake", at.root, &build, "cc", NULL, &lines, err, sizeof err));
    EXPECT_EQ(1, (int)lines.count);
    compile_lines_free(&lines);
    (void)fs_remove_tree(at.root);
}

DESCRIBE(a_build_that_compiles_nothing_recognisable_is_an_error) {
    sandbox at;
    ASSERT_TRUE(make_buildable(&at));
    recipe_build build = delegated();
    build.target_count = 0;
    build.sources = recipe_sources_make;
    char err[512] = "";
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "clang", NULL, NULL, err,
                                     sizeof err));
    compile_lines lines;
    compile_lines_init(&lines);
    EXPECT_FALSE(configure_compile_lines("fake", at.root, &build, "clang", NULL, &lines, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "compiles nothing"));
    compile_lines_free(&lines);
    (void)fs_remove_tree(at.root);
}

/* zlib's part, as FFmpeg's configure needs it: archived, and on LDFLAGS. */
DESCRIBE(a_library_the_configuration_sees_is_built_first) {
    sandbox at;
    ASSERT_TRUE(make_buildable(&at));
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/configure", at.root);
    ASSERT_TRUE(fs_write_file(file, "echo \"$LDFLAGS\" > ldflags.txt\n"));
    char zsource[PATH_MAX_LEN];
    snprintf(zsource, sizeof zsource, "%s/a.c", at.root);

    str_list sources, none;
    str_list_init(&sources);
    str_list_init(&none);
    ASSERT_TRUE(str_list_push(&sources, zsource));
    const configure_library library = {
        .library = "z", .sources = &sources, .includes = &none, .defines = &none, .flags = &none,
        .std = ""};
    const configure_view view = {.libraries = &library, .library_count = 1};
    recipe_build build = delegated();
    build.target_count = 0;
    char err[512] = "";
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "cc", NULL, &view, err, sizeof err));

    snprintf(file, sizeof file, "%s/.molto-libs/libz.a", at.root);
    EXPECT_TRUE(fs_path_exists(file));
    snprintf(file, sizeof file, "%s/ldflags.txt", at.root);
    char *text = fs_read_file(file);
    ASSERT_NOT_NULL(text);
    EXPECT_NOT_NULL(strstr(text, ".molto-libs"));
    free(text);
    str_list_free(&sources);
    (void)fs_remove_tree(at.root);
}

/* xz's part: a library whose own build said what it compiles is compiled with
   those lines, not with a recipe's lists. */
DESCRIBE(a_library_read_from_its_build_is_compiled_with_its_own_lines) {
    sandbox at;
    ASSERT_TRUE(make_buildable(&at));
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/configure", at.root);
    ASSERT_TRUE(fs_write_file(file, "echo \"$CPPFLAGS\" > cppflags.txt\n"));
    char source[PATH_MAX_LEN];
    snprintf(source, sizeof source, "%s/needs.c", at.root);
    ASSERT_TRUE(fs_write_file(source, "#ifndef FROM_ITS_LINE\n#error not its line\n#endif\n"
                                      "int needs(void) { return 1; }\n"));

    str_list sources, none, defines;
    str_list_init(&sources);
    str_list_init(&none);
    str_list_init(&defines);
    ASSERT_TRUE(str_list_push(&sources, source));
    ASSERT_TRUE(str_list_push(&defines, "LZMA_API_STATIC"));
    str_list args[1];
    str_list_init(&args[0]);
    ASSERT_TRUE(str_list_push(&args[0], "-DFROM_ITS_LINE"));
    const configure_library library = {.library = "lzma",
                                       .sources = &sources,
                                       .includes = &none,
                                       .defines = &none,
                                       .flags = &none,
                                       .std = "",
                                       .source_args = args};
    const configure_view view = {.defines = &defines, .libraries = &library, .library_count = 1};
    recipe_build build = delegated();
    build.target_count = 0;
    char err[512] = "";
    ASSERT_TRUE(configure_dependency("fake", at.root, &build, "cc", NULL, &view, err, sizeof err));
    snprintf(file, sizeof file, "%s/.molto-libs/liblzma.a", at.root);
    EXPECT_TRUE(fs_path_exists(file));
    /* What its dependencies define reaches the probes, as the build reads it. */
    snprintf(file, sizeof file, "%s/cppflags.txt", at.root);
    char *text = fs_read_file(file);
    ASSERT_NOT_NULL(text);
    EXPECT_NOT_NULL(strstr(text, "-DLZMA_API_STATIC"));
    free(text);

    str_list_free(&args[0]);
    str_list_free(&sources);
    str_list_free(&defines);
    (void)fs_remove_tree(at.root);
}

/* --- a delegated CMake --- */

#ifndef _WIN32
/* A cmake that writes down how it was asked, and a ninja that answers. */
static bool make_fake_cmake(const sandbox *at, char *cmake, size_t size) {
    char ninja[PATH_MAX_LEN];
    snprintf(cmake, size, "%s/fake-cmake", at->root);
    snprintf(ninja, sizeof ninja, "%s/fake-ninja", at->root);
    if (!fs_write_file(cmake, "#!/bin/sh\n"
                              "if [ \"$1\" = --build ]; then echo built > \"$2/$4.txt\"; exit 0; fi\n"
                              "mkdir -p .molto-cmake\n"
                              "printf '%s\\n' \"$@\" > .molto-cmake/args.txt\n"
                              "echo \"CPATH=$CPATH\" >> .molto-cmake/args.txt\n") ||
        !fs_write_file(ninja, "#!/bin/sh\necho 1.13.2\n"))
        return false;
    const char *chmod_argv[] = {"chmod", "+x", cmake, ninja, NULL};
    char ignored[64];
    if (process_capture(chmod_argv, ignored, sizeof ignored) != 0)
        return false;
    return setenv("MOLTO_CMAKE", cmake, 1) == 0 && setenv("MOLTO_NINJA", ninja, 1) == 0;
}

DESCRIBE(cmake_configures_with_molto_s_compiler_and_what_it_resolved) {
    sandbox at;
    ASSERT_TRUE(moltest_temp_dir("molto_cmake", at.root, sizeof at.root));
    char cmake[PATH_MAX_LEN];
    ASSERT_TRUE(make_fake_cmake(&at, cmake, sizeof cmake));

    recipe_build build = {0};
    build.system = recipe_build_cmake;
    build.via = recipe_via_delegate;
    snprintf(build.args[build.arg_count++], RECIPE_BUILD_ARG_MAX, "-DLWS_WITH_SSL=ON");
    snprintf(build.targets[build.target_count++], RECIPE_BUILD_ARG_MAX, "gen_headers");

    str_list includes;
    str_list_init(&includes);
    ASSERT_TRUE(str_list_push(&includes, "/deps/openssl/include"));
    str_list links;
    str_list_init(&links);
    ASSERT_TRUE(str_list_push(&links, "-L/deps/openssl/lib"));
    ASSERT_TRUE(str_list_push(&links, "-lssl"));
    const configure_view view = {.includes = &includes, .link_flags = &links, .cxx = "c++"};

    char err[512] = "";
    ASSERT_TRUE(configure_dependency("lws", at.root, &build, "cc", NULL, &view, err, sizeof err));

    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/.molto-cmake/args.txt", at.root);
    char *args = fs_read_file(file);
    ASSERT_NOT_NULL(args);
    EXPECT_NOT_NULL(strstr(args, "-G\nNinja\n"));
    EXPECT_NOT_NULL(strstr(args, "-DCMAKE_C_COMPILER=cc\n"));
    /* The C++ compiler too, when the build has one. */
    EXPECT_NOT_NULL(strstr(args, "-DCMAKE_CXX_COMPILER=c++\n"));
    EXPECT_NOT_NULL(strstr(args, "-DLWS_WITH_SSL=ON\n"));
    /* OpenSSL's find module searches <prefix>/include and <prefix>/lib. */
    EXPECT_NOT_NULL(strstr(args, "-DCMAKE_PREFIX_PATH=/deps/openssl\n"));
    EXPECT_NOT_NULL(strstr(args, "-DCMAKE_LIBRARY_PATH=/deps/openssl/lib\n"));
    /* And every include directory reaches every check it compiles. */
    EXPECT_NOT_NULL(strstr(args, "CPATH=/deps/openssl/include\n"));
    free(args);
    snprintf(file, sizeof file, "%s/.molto-cmake/gen_headers.txt", at.root);
    EXPECT_TRUE(fs_path_exists(file));

    str_list_free(&includes);
    str_list_free(&links);
    (void)unsetenv("MOLTO_CMAKE");
    (void)unsetenv("MOLTO_NINJA");
    (void)fs_remove_tree(at.root);
}
#endif

DESCRIBE(a_build_tool_named_outright_is_taken_as_named) {
    ASSERT_EQ(0, setenv("MOLTO_NINJA", "/opt/ninja/ninja", 1));
    resolved_tool ninja;
    char err[256] = "";
    ASSERT_TRUE(tool_resolve_build("ninja", &ninja, err, sizeof err));
    EXPECT_STREQ("/opt/ninja/ninja", ninja.path);
    (void)unsetenv("MOLTO_NINJA");
}

/* --- what a platform package gives a configuration --- */

/* A distribution's package unpacked somewhere: its .pc moved there, its bin
   and its libraries named. */
DESCRIBE(a_platform_tree_is_relocated_for_its_configuration) {
    sandbox at;
    ASSERT_TRUE(moltest_temp_dir("molto_tree", at.root, sizeof at.root));
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof dir, "%s/usr/lib/x86_64-linux-gnu/pkgconfig", at.root);
    ASSERT_TRUE(fs_make_dirs(dir));
    snprintf(dir, sizeof dir, "%s/usr/bin", at.root);
    ASSERT_TRUE(fs_make_dirs(dir));
    char file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s/usr/lib/x86_64-linux-gnu/pkgconfig/wayland-client.pc", at.root);
    ASSERT_TRUE(fs_write_file(file, "prefix=/usr\n"
                                    "libdir=/usr/lib/x86_64-linux-gnu\n"
                                    "includedir=${prefix}/include\n\n"
                                    "Name: Wayland Client\n"
                                    "Cflags: -I${includedir}\n"
                                    "Libs: -L${libdir} -lwayland-client\n"));

    str_list pc, bin, lib;
    str_list_init(&pc);
    str_list_init(&bin);
    str_list_init(&lib);
    ASSERT_TRUE(configure_platform_tree(at.root, &pc, &bin, &lib));
    ASSERT_EQ(1, (int)str_list_count(&pc));
    ASSERT_EQ(1, (int)str_list_count(&bin));
    /* The multiarch directory first, then usr/lib, which holds it. */
    ASSERT_EQ(2, (int)str_list_count(&lib));
    EXPECT_NOT_NULL(strstr(str_list_get(&lib, 0), "usr/lib/x86_64-linux-gnu"));

    snprintf(file, sizeof file, "%s/wayland-client.pc", str_list_get(&pc, 0));
    char *text = fs_read_file(file);
    ASSERT_NOT_NULL(text);
    char expected[PATH_MAX_LEN];
    snprintf(expected, sizeof expected, "prefix=%s/usr\n", at.root);
    EXPECT_NOT_NULL(strstr(text, expected));
    snprintf(expected, sizeof expected, "libdir=%s/usr/lib/x86_64-linux-gnu\n", at.root);
    EXPECT_NOT_NULL(strstr(text, expected));
    /* The fields refer to the variables and are left as they are. */
    EXPECT_NOT_NULL(strstr(text, "Libs: -L${libdir} -lwayland-client\n"));
    free(text);

    /* A host answer has no tree, and gives nothing. */
    ASSERT_TRUE(configure_platform_tree("host", &pc, &bin, &lib));
    EXPECT_EQ(1, (int)str_list_count(&pc));

    str_list_free(&pc);
    str_list_free(&bin);
    str_list_free(&lib);
    (void)fs_remove_tree(at.root);
}
