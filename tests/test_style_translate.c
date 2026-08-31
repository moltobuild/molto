#include <moltest.h>

#include <molto/services/fs_service.h>

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
    ASSERT_TRUE(style_translate_format_text(&config, &backend, "", text, sizeof text,
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
    ASSERT_TRUE(style_translate_format_text(&config, &backend, "", text, sizeof text,
                                            err, sizeof err));

    /* `SortIncludes: true` still parses but is deprecated since LLVM 17, and
       the backend is pinned, so there is no older release to humour. */
    EXPECT_NOT_NULL(strstr(text, "SortIncludes: CaseSensitive"));
    EXPECT_NULL(strstr(text, "SortIncludes: true"));
    /* ReflowComments goes the other way: the `Always` spelling arrived in
       LLVM 20 and clang-format 19 rejects the whole configuration over it, so
       the boolean every version reads is the one to write. */
    EXPECT_NOT_NULL(strstr(text, "ReflowComments: true"));
    EXPECT_NULL(strstr(text, "ReflowComments: Always"));
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
        ASSERT_TRUE(style_translate_format_text(&config, &backend, "", text, sizeof text,
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
    EXPECT_FALSE(style_translate_format_text(&config, &backend, "", text, sizeof text,
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
    EXPECT_FALSE(style_translate_format_text(&config, &backend, "", text, sizeof text,
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
    EXPECT_TRUE(style_translate_format_text(&config, &backend, "", text, sizeof text,
                                            err, sizeof err));
}

MOLTEST(translate_parses_a_c_project_as_c_and_not_as_the_newest_cpp) {
    style_config config;
    style_config_defaults(&config);
    resolved_tool backend = formatter_backend();

    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_format_text(&config, &backend, "", text, sizeof text,
                                            err, sizeof err));

    /* A .h carries no language in its extension, so clang-format assumes the
       newest C++ and reads a C field named `requires` as a requires-clause,
       tearing the declaration across three lines. The oldest dialect is the
       only one where those identifiers are just identifiers. */
    EXPECT_NOT_NULL(strstr(text, "Standard: c++03"));
}

MOLTEST(translate_parses_a_cpp_project_as_the_standard_it_declared) {
    resolved_tool backend = formatter_backend();
    const struct { const char *declared; const char *native; } cases[] = {
        { "c++11",   "Standard: c++11" },
        { "c++17",   "Standard: c++17" },
        { "c++20",   "Standard: c++20" },
        { "gnu++20", "Standard: c++20" },
        { "c++2a",   "Standard: c++20" },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        style_config config;
        style_config_defaults(&config);

        char text[4096] = "";
        char err[256] = "";
        ASSERT_TRUE(style_translate_format_text(&config, &backend, cases[i].declared, text,
                                                sizeof text, err, sizeof err));
        /* A C++20 project means it when it writes `requires`, and formatting it
           as C++03 would misparse the code this exists to protect. */
        EXPECT_NOT_NULL(strstr(text, cases[i].native));
    }
}

MOLTEST(translate_falls_back_to_the_newest_for_a_standard_it_does_not_know) {
    style_config config;
    style_config_defaults(&config);
    resolved_tool backend = formatter_backend();

    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_format_text(&config, &backend, "c++26", text, sizeof text,
                                            err, sizeof err));

    /* An unknown dialect is newer, not older: guessing old would misparse it. */
    EXPECT_NOT_NULL(strstr(text, "Standard: Latest"));
}

MOLTEST(translate_indents_with_tabs_without_aligning_with_them) {
    style_config config;
    style_config_defaults(&config);
    config.style.use_tabs = true;
    resolved_tool backend = formatter_backend();

    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_format_text(&config, &backend, "", text, sizeof text,
                                            err, sizeof err));

    /* The canonical key promises indentation; Always would also put tabs inside
       alignment, which is a different decision the user did not make. */
    EXPECT_NOT_NULL(strstr(text, "UseTab: ForIndentation"));
    EXPECT_NULL(strstr(text, "UseTab: Always"));
}

MOLTEST(translate_does_not_repeat_a_check_the_preset_already_enabled) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_molto; /* already asks for bugprone-* */
    snprintf(config.rules[0].name, LINT_RULE_NAME_MAX, "%s", "bugprone");
    config.rules[0].severity = lint_severity_warn;
    config.rule_count = 1;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    /* clang-tidy tolerated the duplicate, which is why it went unnoticed; a
       person reading the generated file to debug their configuration does not,
       and doubts the generator first. */
    const char *first = strstr(text, "bugprone-*");
    ASSERT_NOT_NULL(first);
    EXPECT_NULL(strstr(first + 1, "bugprone-*"));
}

MOLTEST(translate_still_subtracts_a_check_the_preset_enabled) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_molto;
    snprintf(config.rules[0].name, LINT_RULE_NAME_MAX, "%s", "bugprone");
    config.rules[0].severity = lint_severity_off;
    config.rule_count = 1;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    /* Turning one off is the case where the check must be emitted again: the
       minus is what subtracts what the preset put there. */
    EXPECT_NOT_NULL(strstr(text, "-bugprone-*"));
}

MOLTEST(translate_makes_a_preset_check_fatal_when_a_rule_raises_it) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_molto;
    snprintf(config.rules[0].name, LINT_RULE_NAME_MAX, "%s", "bugprone");
    config.rules[0].severity = lint_severity_error;
    config.rule_count = 1;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    /* Not appending it to Checks must not cost it its severity: the rule was
       raised to error and WarningsAsErrors is the only place that says so. */
    EXPECT_NOT_NULL(strstr(text, "WarningsAsErrors: 'bugprone-*'"));
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
    ASSERT_TRUE(moltest_temp_dir("molto_translate", root, sizeof root));

    style_config config;
    style_config_defaults(&config);
    resolved_tool backend = formatter_backend();

    char path[STYLE_CONFIG_PATH_MAX] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_format(root, &config, &backend, "", path, sizeof path,
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
    (void)fs_remove_tree(root);
}

MOLTEST(translate_can_refuse_one_check_without_giving_up_its_family) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_molto; /* asks for bugprone-* */
    snprintf(config.rules[0].name, LINT_RULE_NAME_MAX, "%s", "swappable_parameters");
    config.rules[0].severity = lint_severity_off;
    snprintf(config.rules[1].name, LINT_RULE_NAME_MAX, "%s", "spurious_wakeup");
    config.rules[1].severity = lint_severity_off;
    config.rule_count = 2;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    /* A family is too coarse a unit to turn one noisy check off: without these
       names the only way to silence them is dropping bugprone entirely, and
       with it the checks that do find bugs. */
    EXPECT_NOT_NULL(strstr(text, "-bugprone-easily-swappable-parameters"));
    EXPECT_NOT_NULL(strstr(text, "-bugprone-spuriously-wake-up-functions"));
    EXPECT_NOT_NULL(strstr(text, "bugprone-*"));
}

/* clang-tidy composes a Checks list on top of its own default rather than
   replacing it, and that default is clang-analyzer-* — the path-sensitive
   analyzer the molto preset deliberately leaves out. A list that does not open
   by clearing it runs an analysis nobody configured, at a cost nobody agreed
   to, and its contents change with the version of clang-tidy on the machine. */
MOLTEST(translate_opens_the_check_list_by_clearing_the_backend_default) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_molto;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    EXPECT_NOT_NULL(strstr(text, "Checks: '-*,clang-diagnostic-*,bugprone-*"));
    /* The preset says nothing about the analyzer, so neither does the file. */
    EXPECT_NULL(strstr(text, "clang-analyzer"));
}

/* The same for a project that asked for no preset at all: an empty canonical
   model must translate to an empty analysis, not to whichever checks the
   backend happens to run when it is told nothing. */
MOLTEST(translate_clears_the_default_even_with_no_preset_and_no_rules) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_none;
    config.rule_count = 0;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    EXPECT_NOT_NULL(strstr(text, "Checks: '-*'"));
}

/* The analyzer separates its families with a dot. Spelled with the hyphen the
   rest of clang-tidy uses, the pattern matches no check at all, and the rule
   this project documents turns nothing on. */
MOLTEST(translate_spells_an_analyzer_family_the_way_the_analyzer_does) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_none;
    snprintf(config.rules[0].name, LINT_RULE_NAME_MAX, "%s", "security");
    config.rules[0].severity = lint_severity_warn;
    config.rule_count = 1;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    EXPECT_NOT_NULL(strstr(text, "clang-analyzer-security.*"));
    EXPECT_NULL(strstr(text, "clang-analyzer-security-*"));

    /* And without the check that asks for the C11 Annex K functions glibc does
       not ship, which fires on every call to the C library. */
    EXPECT_NOT_NULL(strstr(text,
                           "-clang-analyzer-security.insecureAPI."
                           "DeprecatedOrUnsafeBufferHandling"));
}

/* The path-sensitive analyzer, asked for by name. What it covers is the
   families that say something about C on this platform — not osx, not
   cplusplus — and not unix.Stream, which reads every `while(fread(...))` in
   this repository as a read past the end of the file. */
MOLTEST(translate_asks_the_analyzer_only_for_what_says_something_about_c) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_molto;
    snprintf(config.rules[0].name, LINT_RULE_NAME_MAX, "%s", "dataflow");
    config.rules[0].severity = lint_severity_warn;
    config.rule_count = 1;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    EXPECT_NOT_NULL(strstr(text, "clang-analyzer-core.*"));
    EXPECT_NOT_NULL(strstr(text, "clang-analyzer-unix.*"));
    EXPECT_NOT_NULL(strstr(text, "clang-analyzer-valist.*"));
    EXPECT_NOT_NULL(strstr(text, "clang-analyzer-deadcode.*"));
    EXPECT_NOT_NULL(strstr(text, "-clang-analyzer-unix.Stream"));
    /* Families that describe another language or another platform stay out. */
    EXPECT_NULL(strstr(text, "clang-analyzer-osx"));
    EXPECT_NULL(strstr(text, "clang-analyzer-cplusplus"));
}

/* Turning a rule off means every check it names, one minus each. A single
   minus in front of the list would negate its first element and leave the rest
   running — and the check the rule already subtracts must not come back on,
   which is what re-emitting it without its minus would do. */
MOLTEST(translate_turns_a_rule_off_check_by_check) {
    lint_config config;
    lint_config_defaults(&config);
    config.preset = style_preset_none;
    snprintf(config.rules[0].name, LINT_RULE_NAME_MAX, "%s", "dataflow");
    config.rules[0].severity = lint_severity_off;
    config.rule_count = 1;

    resolved_tool backend = linter_backend();
    char text[4096] = "";
    char err[256] = "";
    ASSERT_TRUE(style_translate_lint_text(&config, &backend, text, sizeof text,
                                          err, sizeof err));

    EXPECT_NOT_NULL(strstr(text, "-clang-analyzer-core.*"));
    EXPECT_NOT_NULL(strstr(text, "-clang-analyzer-unix.*"));
    EXPECT_NOT_NULL(strstr(text, "-clang-analyzer-valist.*"));
    EXPECT_NOT_NULL(strstr(text, "-clang-analyzer-deadcode.*"));
    /* The one the rule subtracts is off either way, so it is not mentioned at
       all — mentioned without its minus, it would be the only one left on. */
    EXPECT_NULL(strstr(text, "Stream"));
}
