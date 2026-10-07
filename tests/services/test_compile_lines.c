#include <moltest.h>

#include <molto/services/compile_lines.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reading what upstream's build compiles (RFC-0025), from the text `make -n`
   prints and from a compile_commands.json. */

static const compile_drivers DRIVERS = {.cc = "/opt/cc/bin/clang", .nasm = "/opt/nasm/nasm"};

static bool has_arg(const compile_line *line, const char *arg) {
    for (size_t i = 0; i < str_list_count(&line->args); i++) {
        if (strcmp(str_list_get(&line->args, i), arg) == 0)
            return true;
    }
    return false;
}

DESCRIBE(words_split_as_sh_splits_them) {
    str_list words;
    str_list_init(&words);
    ASSERT_TRUE(compile_lines_split("cc -DA='x y' -DB=\"q\\\"r\" a\\ b  -c", &words));
    ASSERT_EQ(5, (int)str_list_count(&words));
    EXPECT_STREQ("-DA=x y", str_list_get(&words, 1));
    EXPECT_STREQ("-DB=q\"r", str_list_get(&words, 2));
    EXPECT_STREQ("a b", str_list_get(&words, 3));
    str_list_free(&words);
}

DESCRIBE(a_make_dry_run_gives_each_file_and_its_arguments) {
    const char *output =
        "mkdir -p libavutil/\n"
        "/opt/cc/bin/clang -I. -I./ -DBUILDING_avutil -std=c17 -O3 -MMD -MF libavutil/a.d "
        "-MT libavutil/a.o -c -o libavutil/a.o libavutil/a.c\n"
        "/opt/cc/bin/clang -I. -include ./config.h -MMD -MF x.d -c -o libavutil/aarch64/b.o "
        "libavutil/aarch64/b.S\n"
        "/opt/nasm/nasm -f elf64 -I./ -Pconfig.asm -MD libavutil/x86/c.d -o libavutil/x86/c.o "
        "libavutil/x86/c.asm\n"
        "ar rcD libavutil/libavutil.a libavutil/a.o\n"
        "ranlib libavutil/libavutil.a\n"
        "./ffbuild/version.sh . libavutil/ffversion.h\n";
    compile_lines lines;
    compile_lines_init(&lines);
    char err[256] = "";
    ASSERT_TRUE(compile_lines_from_make(output, "/src/ff", &DRIVERS, &lines, err, sizeof err));
    ASSERT_EQ(3, (int)lines.count);

    EXPECT_STREQ("libavutil/a.c", lines.lines[0].source);
    EXPECT_TRUE(has_arg(&lines.lines[0], "-I/src/ff"));
    EXPECT_TRUE(has_arg(&lines.lines[0], "-I/src/ff/"));
    EXPECT_TRUE(has_arg(&lines.lines[0], "-DBUILDING_avutil"));
    EXPECT_TRUE(has_arg(&lines.lines[0], "-O3"));
    /* What molto chooses itself is gone: the input, -c, -o and the depfile. */
    EXPECT_FALSE(has_arg(&lines.lines[0], "-c"));
    EXPECT_FALSE(has_arg(&lines.lines[0], "-MMD"));
    EXPECT_FALSE(has_arg(&lines.lines[0], "-MF"));
    EXPECT_FALSE(has_arg(&lines.lines[0], "libavutil/a.o"));
    EXPECT_FALSE(has_arg(&lines.lines[0], "libavutil/a.c"));

    EXPECT_STREQ("libavutil/aarch64/b.S", lines.lines[1].source);
    EXPECT_TRUE(has_arg(&lines.lines[1], "-include"));
    /* config.h is not on disk here, so it stays as written. */
    EXPECT_TRUE(has_arg(&lines.lines[1], "./config.h"));

    EXPECT_STREQ("libavutil/x86/c.asm", lines.lines[2].source);
    EXPECT_TRUE(has_arg(&lines.lines[2], "-f"));
    EXPECT_TRUE(has_arg(&lines.lines[2], "elf64"));
    EXPECT_TRUE(has_arg(&lines.lines[2], "-I/src/ff/"));
    EXPECT_FALSE(has_arg(&lines.lines[2], "-MD"));
    EXPECT_FALSE(has_arg(&lines.lines[2], "libavutil/x86/c.d"));
    compile_lines_free(&lines);
}

DESCRIBE(a_line_by_another_program_is_not_a_compile_line) {
    const char *output = "gcc -c -o a.o a.c\n"
                         "printf 'CC\\t%s\\n' a.o; /opt/cc/bin/clang -c -o b.o b.c\n"
                         "/opt/cc/bin/clang -E a.c\n";
    compile_lines lines;
    compile_lines_init(&lines);
    char err[256] = "";
    ASSERT_TRUE(compile_lines_from_make(output, "/src/x", &DRIVERS, &lines, err, sizeof err));
    /* Only b.c: gcc is not the compiler molto passed, and -E compiles nothing. */
    ASSERT_EQ(1, (int)lines.count);
    EXPECT_STREQ("b.c", lines.lines[0].source);
    compile_lines_free(&lines);
}

DESCRIBE(a_recursive_make_anchors_where_it_entered) {
    const char *output = "make[1]: Entering directory '/src/x/lib'\n"
                         "/opt/cc/bin/clang -I../include -c -o a.o a.c\n"
                         "make[1]: Leaving directory '/src/x/lib'\n"
                         "/opt/cc/bin/clang -c -o m.o main.c\n";
    compile_lines lines;
    compile_lines_init(&lines);
    char err[256] = "";
    ASSERT_TRUE(compile_lines_from_make(output, "/src/x", &DRIVERS, &lines, err, sizeof err));
    ASSERT_EQ(2, (int)lines.count);
    EXPECT_STREQ("lib/a.c", lines.lines[0].source);
    EXPECT_TRUE(has_arg(&lines.lines[0], "-I/src/x/include"));
    EXPECT_STREQ("main.c", lines.lines[1].source);
    compile_lines_free(&lines);
}

DESCRIBE(a_file_outside_the_source_is_refused) {
    const char *output = "/opt/cc/bin/clang -c -o a.o ../../etc/a.c\n";
    compile_lines lines;
    compile_lines_init(&lines);
    char err[256] = "";
    EXPECT_FALSE(compile_lines_from_make(output, "/src/x", &DRIVERS, &lines, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "not inside"));
    compile_lines_free(&lines);
}

DESCRIBE(a_compile_database_is_read_entry_by_entry) {
    const char *json =
        "[{\"directory\": \"/src/lws/.molto-cmake\","
        "  \"command\": \"/opt/cc/bin/clang -DLWS -I/src/lws/include -o a.o -c /src/lws/lib/a.c\","
        "  \"file\": \"/src/lws/lib/a.c\"},"
        " {\"directory\": \"/src/lws/.molto-cmake\","
        "  \"arguments\": [\"/opt/cc/bin/clang\", \"-Iinclude\", \"-c\", \"../lib/b.c\"],"
        "  \"file\": \"../lib/b.c\"}]";
    compile_lines lines;
    compile_lines_init(&lines);
    char err[256] = "";
    ASSERT_TRUE(compile_lines_from_database(json, "/src/lws", &DRIVERS, &lines, err, sizeof err));
    ASSERT_EQ(2, (int)lines.count);
    EXPECT_STREQ("lib/a.c", lines.lines[0].source);
    EXPECT_TRUE(has_arg(&lines.lines[0], "-DLWS"));
    EXPECT_STREQ("lib/b.c", lines.lines[1].source);
    EXPECT_TRUE(has_arg(&lines.lines[1], "-I/src/lws/.molto-cmake/include"));
    compile_lines_free(&lines);
}

DESCRIBE(the_list_reads_back_as_it_was_written) {
    char dir[64];
    ASSERT_TRUE(moltest_temp_dir("molto_lines", dir, sizeof dir));
    char path[128];
    snprintf(path, sizeof path, "%s/.molto-sources", dir);

    compile_lines lines;
    compile_lines_init(&lines);
    char err[256] = "";
    ASSERT_TRUE(compile_lines_from_make("/opt/cc/bin/clang -DQ=\"a b\" -c -o a.o a.c\n", "/s",
                                        &DRIVERS, &lines, err, sizeof err));
    ASSERT_TRUE(compile_lines_write(&lines, path));

    compile_lines back;
    compile_lines_init(&back);
    ASSERT_TRUE(compile_lines_read(path, &back));
    ASSERT_EQ(1, (int)back.count);
    EXPECT_STREQ("a.c", back.lines[0].source);
    EXPECT_TRUE(has_arg(&back.lines[0], "-DQ=a b"));
    compile_lines_free(&lines);
    compile_lines_free(&back);
    (void)fs_remove_tree(dir);
}
