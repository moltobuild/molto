#include <moltest.h>

#include <molto/exit_code.h>
#include <molto/services/fmt_service.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A stand-in for clang-format, so these exercise Molto's side of the contract
   without depending on what this machine has installed. The stub logs its argv
   and behaves like the real tool in whichever mode it was asked for. */
typedef struct {
    char root[64];     /* the workspace */
    char tools[64];    /* where the stub lives */
    char program[128];
    char log[128];
    char previous[4096];
    bool had_previous;
} fmt_fixture;

/*
 * Behaves like clang-format: --dry-run reports and exits 1, -i rewrites, and a
 * plain invocation prints the formatted file. "Formatting" here is replacing
 * two leading spaces with four, which is enough to be observable.
 *
 * Written in C rather than as a `#!/bin/sh` file, because a shebang is a thing
 * the kernel honours and CreateProcess does not.
 */
static bool reindent(const char *text, char *out, size_t size) {
    size_t written = 0;
    bool at_line_start = true;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        /* Two leading spaces become four, and only when they are the whole
           indentation: formatting an already formatted file has to be a no-op,
           or nothing that follows means anything. */
        if (at_line_start && cursor[0] == ' ' && cursor[1] == ' ' && cursor[2] != ' ') {
            if (written + 4 >= size)
                return false;
            memcpy(out + written, "    ", 4);
            written += 4;
            cursor++;
            at_line_start = false;
            continue;
        }
        if (written + 1 >= size)
            return false;
        at_line_start = *cursor == '\n';
        out[written++] = *cursor;
    }
    out[written] = '\0';
    return true;
}

MOLTEST_FAKE(fake_clang_format) {
    const char *log = moltest_fake_setting("log");
    FILE *file;
    if (log != NULL)
        (void)moltest_log_argv(log, NULL, argc, argv);

    enum { mode_print, mode_check, mode_write } mode = mode_print;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0)
            mode = mode_check;
        else if (strcmp(argv[i], "-i") == 0)
            mode = mode_write;
        else if (strncmp(argv[i], "--style=", 8) != 0 && strcmp(argv[i], "--Werror") != 0)
            path = argv[i];
    }
    if (path == NULL)
        return 0;

    char *text = fs_read_file(path);
    if (text == NULL)
        return 0;

    char formatted[16384];
    const bool ok = reindent(text, formatted, sizeof formatted);
    const bool unchanged = ok && strcmp(formatted, text) == 0;
    free(text);
    if (!ok)
        return 0;

    if (unchanged) {
        if (mode == mode_print)
            printf("%s", formatted);
        return 0;
    }

    switch (mode) {
    case mode_check:
        fprintf(stderr, "%s:1:3: error: code should be clang-formatted"
                        " [-Wclang-format-violations]\n", path);
        return 1;
    case mode_write:
        if ((file = fopen(path, "wb")) != NULL) {
            fputs(formatted, file);
            (void)fclose(file);
        }
        return 0;
    case mode_print:
        printf("%s", formatted);
        return 0;
    }
    return 0;
}

static bool write_source(const char *root, const char *relative, const char *body) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", root, relative);

    char directory[256];
    snprintf(directory, sizeof directory, "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (!fs_make_dirs(directory))
            return false;
    }
    return fs_write_file(path, body);
}

static bool fixture_setup(fmt_fixture *fixture) {
    if (!moltest_temp_dir("molto_fmt_bin", fixture->tools, sizeof fixture->tools) || !moltest_temp_dir("molto_fmt_ws", fixture->root, sizeof fixture->root))
        return false;

    snprintf(fixture->program, sizeof fixture->program, "%s/clang-format", fixture->tools);
    snprintf(fixture->log, sizeof fixture->log, "%s/calls", fixture->tools);

    const char *existing = getenv("MOLTO_CLANG_FORMAT");
    fixture->had_previous = existing != NULL;
    if (existing != NULL)
        snprintf(fixture->previous, sizeof fixture->previous, "%s", existing);

    char spec[1024];
    if (snprintf(spec, sizeof spec, "set log %s\nbehave fake_clang_format\n", fixture->log)
        >= (int)sizeof spec)
        return false;

    return moltest_fake_program(fixture->program, spec, fixture->program,
                                sizeof fixture->program)
        && setenv("MOLTO_CLANG_FORMAT", fixture->program, 1) == 0
        && write_source(fixture->root, "Project.toml",
                        "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n")
        /* Two-space indentation, which the stub turns into four. */
        && write_source(fixture->root, "src/main.c",
                        "int main(void) {\n  return 0;\n}\n")
        && write_source(fixture->root, "include/demo.h",
                        "#ifndef DEMO_H\n  int demo(void);\n#endif\n");
}

static void fixture_teardown(fmt_fixture *fixture) {
    if (fixture->had_previous)
        (void)setenv("MOLTO_CLANG_FORMAT", fixture->previous, 1);
    else
        (void)unsetenv("MOLTO_CLANG_FORMAT");
    char cmd[256];
    (void)fs_remove_tree(fixture->root);
    (void)fs_remove_tree(fixture->tools);
}

static char *read_source(const char *root, const char *relative) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", root, relative);
    return fs_read_file(path);
}

static int run_fmt(const fmt_fixture *fixture, fmt_mode mode, FILE *diff_stream,
                   fmt_result *result) {
    const fmt_request request = {
        .mode = mode,
        .refresh_tools = false,
        .diff_stream = diff_stream,
    };
    fmt_result_init(result);
    return fmt_project(fixture->root, &request, result);
}

MOLTEST(fmt_formats_the_sources_in_place) {
    fmt_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    fmt_result result;
    ASSERT_EQ(exit_ok, run_fmt(&fixture, fmt_mode_write, NULL, &result));

    char *body = read_source(fixture.root, "src/main.c");
    ASSERT_NOT_NULL(body);
    EXPECT_NOT_NULL(strstr(body, "    return 0;"));
    free(body);

    fmt_result_free(&result);
    fixture_teardown(&fixture);
}

MOLTEST(fmt_also_formats_the_headers) {
    fmt_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    fmt_result result;
    ASSERT_EQ(exit_ok, run_fmt(&fixture, fmt_mode_write, NULL, &result));

    /* RFC-0005 is explicit: style applies to a .h as much as to a .c, and a
       header nobody includes still has to be formatted. */
    char *body = read_source(fixture.root, "include/demo.h");
    ASSERT_NOT_NULL(body);
    EXPECT_NOT_NULL(strstr(body, "    int demo(void);"));
    free(body);

    fmt_result_free(&result);
    fixture_teardown(&fixture);
}

MOLTEST(fmt_check_reports_what_would_change_and_writes_nothing) {
    fmt_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    fmt_result result;
    ASSERT_EQ(exit_ok, run_fmt(&fixture, fmt_mode_check, NULL, &result));

    EXPECT_EQ(2, (int)str_list_count(&result.changed));
    EXPECT_TRUE(diagnostic_list_count(&result.diagnostics) > 0);

    /* The point of --check is that a CI run learns what is wrong without the
       working tree moving under it. */
    char *body = read_source(fixture.root, "src/main.c");
    ASSERT_NOT_NULL(body);
    EXPECT_NOT_NULL(strstr(body, "  return 0;"));
    EXPECT_NULL(strstr(body, "    return 0;"));
    free(body);

    fmt_result_free(&result);
    fixture_teardown(&fixture);
}

MOLTEST(fmt_diff_prints_a_diff_and_writes_nothing) {
    fmt_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    FILE *scratch = tmpfile();
    ASSERT_NOT_NULL(scratch);

    fmt_result result;
    ASSERT_EQ(exit_ok, run_fmt(&fixture, fmt_mode_diff, scratch, &result));

    rewind(scratch);
    char diff[4096] = "";
    size_t read = fread(diff, 1, sizeof diff - 1, scratch);
    diff[read] = '\0';
    fclose(scratch);

    EXPECT_NOT_NULL(strstr(diff, "--- a/src/main.c"));
    EXPECT_NOT_NULL(strstr(diff, "-  return 0;"));
    EXPECT_NOT_NULL(strstr(diff, "+    return 0;"));
    EXPECT_EQ(2, (int)str_list_count(&result.changed));

    char *body = read_source(fixture.root, "src/main.c");
    ASSERT_NOT_NULL(body);
    EXPECT_NOT_NULL(strstr(body, "  return 0;"));
    free(body);

    fmt_result_free(&result);
    fixture_teardown(&fixture);
}

MOLTEST(fmt_reports_nothing_for_a_project_already_formatted) {
    fmt_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    ASSERT_TRUE(write_source(fixture.root, "src/main.c",
                             "int main(void) {\n    return 0;\n}\n"));
    ASSERT_TRUE(write_source(fixture.root, "include/demo.h",
                             "#ifndef DEMO_H\n    int demo(void);\n#endif\n"));

    fmt_result result;
    ASSERT_EQ(exit_ok, run_fmt(&fixture, fmt_mode_check, NULL, &result));
    EXPECT_EQ(0, (int)str_list_count(&result.changed));

    fmt_result_free(&result);
    fixture_teardown(&fixture);
}

MOLTEST(fmt_skips_the_paths_format_json_excludes) {
    fmt_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    ASSERT_TRUE(write_source(fixture.root, "src/vendor/third.c",
                             "int third(void) {\n  return 1;\n}\n"));
    ASSERT_TRUE(write_source(fixture.root, "format.json",
                             "{\"exclude\": [\"src/vendor/**\"]}\n"));

    fmt_result result;
    ASSERT_EQ(exit_ok, run_fmt(&fixture, fmt_mode_write, NULL, &result));

    char *body = read_source(fixture.root, "src/vendor/third.c");
    ASSERT_NOT_NULL(body);
    EXPECT_NOT_NULL(strstr(body, "  return 1;"));
    free(body);

    fmt_result_free(&result);
    fixture_teardown(&fixture);
}

MOLTEST(fmt_generates_the_config_under_bin_and_not_in_the_tree) {
    fmt_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    fmt_result result;
    ASSERT_EQ(exit_ok, run_fmt(&fixture, fmt_mode_write, NULL, &result));

    char generated[256];
    snprintf(generated, sizeof generated, "%s/.bin/style/clang-format.yaml", fixture.root);
    EXPECT_TRUE(fs_path_exists(generated));

    /* A repository using Molto must not accumulate .clang-format files. */
    char stray[256];
    snprintf(stray, sizeof stray, "%s/.clang-format", fixture.root);
    EXPECT_FALSE(fs_path_exists(stray));

    /* And the backend is told where the generated one is. */
    char *log = fs_read_file(fixture.log);
    ASSERT_NOT_NULL(log);
    EXPECT_NOT_NULL(strstr(log, "--style=file:"));
    free(log);

    fmt_result_free(&result);
    fixture_teardown(&fixture);
}

MOLTEST(fmt_refuses_a_configuration_it_cannot_translate) {
    fmt_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    ASSERT_TRUE(write_source(fixture.root, "format.json",
                             "{\"style\": {\"brace_style\": \"whitesmiths\"}}\n"));

    fmt_result result;
    /* Failing closed: a style the user did not ask for, silently, is worse. */
    EXPECT_EQ(exit_invalid_manifest, run_fmt(&fixture, fmt_mode_write, NULL, &result));

    fmt_result_free(&result);
    fixture_teardown(&fixture);
}
