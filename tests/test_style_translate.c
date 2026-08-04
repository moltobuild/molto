#include <moltest.h>

#include <molto/services/style_translate.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What pickup reports for the tools this project would run. */
static resolved_tool formatter_backend(void) {
    resolved_tool tool;
    memset(&tool, 0, sizeof tool);
    snprintf(tool.name, sizeof tool.name, "%s", "clang-format");
    snprintf(tool.path, sizeof tool.path, "%s", "/opt/llvm/bin/clang-format");
    snprintf(tool.version, sizeof tool.version, "%s", "clang-format version 22.1.8");
    return tool;
}

static resolved_tool linter_backend(void) {
    resolved_tool tool;
    memset(&tool, 0, sizeof tool);
    snprintf(tool.name, sizeof tool.name, "%s", "clang-tidy");
    snprintf(tool.path, sizeof tool.path, "%s", "/opt/llvm/bin/clang-tidy");
    snprintf(tool.version, sizeof tool.version, "%s", "LLVM version 22.1.8");
    return tool;
}

MOLTEST(translate_maps_every_canonical_key_to_clang_format) {
    style_config config;
    style_config_defaults(&config);
    resolved_tool backend = formatter_backend();

    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_format_text(&config, &backend, text, sizeof text,
                                            err, sizeof err));

    /* Each canonical key has to reach the backend; one that quietly did not
       would leave the user with a style they did not ask for. */
    EXPECT_NOT_NULL(strstr(text, "IndentWidth: 4"));
    EXPECT_NOT_NULL(strstr(text, "ColumnLimit: 100"));
    EXPECT_NOT_NULL(strstr(text, "UseTab: Never"));
    EXPECT_NOT_NULL(strstr(text, "BreakBeforeBraces: Attach"));
    EXPECT_NOT_NULL(strstr(text, "PointerAlignment: Right"));
    EXPECT_NOT_NULL(strstr(text, "SpaceBeforeParens: Never"));
}

MOLTEST(translate_emits_the_modern_enum_and_not_the_legacy_boolean) {
    style_config config;
    style_config_defaults(&config);
    resolved_tool backend = formatter_backend();

    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_format_text(&config, &backend, text, sizeof text,
                                            err, sizeof err));

    /* `SortIncludes: true` still parses but is deprecated since LLVM 17, and
       the backend is pinned, so there is no older release to humour. */
    EXPECT_NOT_NULL(strstr(text, "SortIncludes: CaseSensitive"));
    EXPECT_NULL(strstr(text, "SortIncludes: true"));
    EXPECT_NOT_NULL(strstr(text, "ReflowComments: Always"));
}

MOLTEST(translate_maps_each_brace_style_to_its_own_backend_value) {
    resolved_tool backend = formatter_backend();
    const struct { brace_style style; const char *native; } cases[] = {
        { brace_style_attach, "BreakBeforeBraces: Attach" },
        { brace_style_break,  "BreakBeforeBraces: Stroustrup" },
        { brace_style_linux,  "BreakBeforeBraces: Linux" },
        { brace_style_allman, "BreakBeforeBraces: Allman" },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        style_config config;
        style_config_defaults(&config);
        config.style.braces = cases[i].style;

        char text[4096] = "";
        char err[256] = "";
        ASSERT_TRUE(style_translate_format_text(&config, &backend, text, sizeof text,
                                                err, sizeof err));
        EXPECT_NOT_NULL(strstr(text, cases[i].native));
    }
}

MOLTEST(translate_refuses_a_backend_it_cannot_speak_for) {
    style_config config;
    style_config_defaults(&config);
    snprintf(config.backend, sizeof config.backend, "%s", "uncrustify@0.78.1");
    resolved_tool backend = formatter_backend();

    char text[4096] = "";
    char err[256] = "";
    /* RFC-0005: name the option and the backend, rather than dropping it. */
    EXPECT_FALSE(style_translate_format_text(&config, &backend, text, sizeof text,
                                             err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "uncrustify@0.78.1"));
    EXPECT_NOT_NULL(strstr(err, "clang-format"));
}

MOLTEST(translate_refuses_a_pin_this_machine_does_not_have) {
    style_config config;
    style_config_defaults(&config);
    snprintf(config.backend, sizeof config.backend, "%s", "clang-format@18.1.8");
    resolved_tool backend = formatter_backend(); /* 22.1.8 */

    char text[4096] = "";
    char err[256] = "";
    /* Different releases format the same input differently, so an unmet pin is
       two developers generating different diffs from one file. */
    EXPECT_FALSE(style_translate_format_text(&config, &backend, text, sizeof text,
                                             err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "18.1.8"));
    EXPECT_NOT_NULL(strstr(err, "22.1.8"));
}

MOLTEST(translate_accepts_a_pin_that_matches) {
    style_config config;
    style_config_defaults(&config);
    snprintf(config.backend, sizeof config.backend, "%s", "clang-format@22.1.8");
    resolved_tool backend = formatter_backend();

    char text[4096] = "";
    char err[256] = "";
    EXPECT_TRUE(style_translate_format_text(&config, &backend, text, sizeof text,
                                            err, sizeof err));
}

MOLTEST(translate_builds_the_check_lists_from_the_rules) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_none;
    snprintf(config.rules[0].name, LINT_RULE_NAME_MAX, "%s", "bugprone");
    config.rules[0].severity = lint_severity_error;
    snprintf(config.rules[1].name, LINT_RULE_NAME_MAX, "%s", "readability_magic_numbers");
    config.rules[1].severity = lint_severity_warn;
    snprintf(config.rules[2].name, LINT_RULE_NAME_MAX, "%s", "modernize");
    config.rules[2].severity = lint_severity_off;
    config.rule_count = 3;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    /* An off rule is subtracted with a minus, and only the error ones are
       fatal: RFC-0005 says a warning reports and the command still succeeds. */
    EXPECT_NOT_NULL(strstr(text, "bugprone-*"));
    EXPECT_NOT_NULL(strstr(text, "readability-magic-numbers"));
    EXPECT_NOT_NULL(strstr(text, "-modernize-*"));
    EXPECT_NOT_NULL(strstr(text, "WarningsAsErrors: 'bugprone-*'"));
}

MOLTEST(translate_refuses_a_rule_it_cannot_express) {
    lint_config config;
    lint_config_defaults(&config);
    snprintf(config.rules[0].name, LINT_RULE_NAME_MAX, "%s", "no_such_rule");
    config.rules[0].severity = lint_severity_error;
    config.rule_count = 1;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    EXPECT_FALSE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                           err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "no_such_rule"));
    EXPECT_NOT_NULL(strstr(err, "clang-tidy"));
}

MOLTEST(translate_writes_the_config_under_bin_and_not_in_the_tree) {
    char root[64];
    snprintf(root, sizeof root, "%s", "/tmp/molto_translate_XXXXXX");
    ASSERT_NOT_NULL(mkdtemp(root));

    style_config config;
    style_config_defaults(&config);
    resolved_tool backend = formatter_backend();

    char path[STYLE_CONFIG_PATH_MAX] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_format(root, &config, &backend, path, sizeof path,
                                       err, sizeof err));

    /* Generated files are machine-owned: a repository using Molto must not
       accumulate .clang-format files. */
    EXPECT_NOT_NULL(strstr(path, "/.bin/style/"));
    char stray[128];
    snprintf(stray, sizeof stray, "%s/.clang-format", root);
    EXPECT_FALSE(access(stray, F_OK) == 0);

    FILE *written = fopen(path, "r");
    ASSERT_NOT_NULL(written);
    char first[128] = "";
    ASSERT_NOT_NULL(fgets(first, sizeof first, written));
    fclose(written);
    EXPECT_NOT_NULL(strstr(first, "Generated by molto"));

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}
