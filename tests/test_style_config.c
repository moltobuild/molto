#include <moltest.h>

#include <molto/project/style_config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A workspace to put configuration files in. */
static bool workspace_setup(char *root, size_t root_size) {
    snprintf(root, root_size, "%s", "/tmp/molto_style_XXXXXX");
    return mkdtemp(root) != NULL;
}

static void workspace_teardown(const char *root) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    (void)system(cmd);
}

MOLTEST(style_config_uses_the_defaults_when_the_file_is_absent) {
    char root[64];
    ASSERT_TRUE(workspace_setup(root, sizeof root));

    style_config config;
    char err[256] = "";
    /* An absent configuration means the defaults, not a failure. */
    ASSERT_TRUE(style_config_load(root, &config, err, sizeof err));
    EXPECT_EQ(4, config.style.indent_width);
    EXPECT_EQ(100, config.style.line_width);
    EXPECT_EQ(brace_style_attach, config.style.braces);
    EXPECT_EQ(pointer_alignment_right, config.style.pointers);
    EXPECT_TRUE(config.style.sort_includes);
    EXPECT_FALSE(config.style.use_tabs);
    EXPECT_EQ(style_preset_molto, config.preset);
    EXPECT_STREQ("", config.backend);

    workspace_teardown(root);
}

MOLTEST(style_config_reads_every_canonical_key) {
    style_config config;
    style_config_defaults(&config);
    char err[256] = "";

    ASSERT_TRUE(style_config_parse(
        "{\n"
        "  \"backend\": \"clang-format@22.1.8\",\n"
        "  \"preset\": \"none\",\n"
        "  \"exclude\": [\"vendor/*\"],\n"
        "  \"style\": {\n"
        "    \"indent_width\": 2,\n"
        "    \"use_tabs\": true,\n"
        "    \"line_width\": 80,\n"
        "    \"brace_style\": \"allman\",\n"
        "    \"pointer_alignment\": \"left\",\n"
        "    \"sort_includes\": false,\n"
        "    \"space_before_paren\": true,\n"
        "    \"column_limit_comments\": false\n"
        "  }\n"
        "}\n", &config, err, sizeof err));

    EXPECT_STREQ("clang-format@22.1.8", config.backend);
    EXPECT_EQ(style_preset_none, config.preset);
    EXPECT_EQ(2, config.style.indent_width);
    EXPECT_TRUE(config.style.use_tabs);
    EXPECT_EQ(80, config.style.line_width);
    EXPECT_EQ(brace_style_allman, config.style.braces);
    EXPECT_EQ(pointer_alignment_left, config.style.pointers);
    EXPECT_FALSE(config.style.sort_includes);
    EXPECT_TRUE(config.style.space_before_paren);
    EXPECT_FALSE(config.style.column_limit_comments);
    EXPECT_EQ(1, (int)config.paths.exclude_count);
}

MOLTEST(style_config_refuses_a_key_it_does_not_know) {
    style_config config;
    style_config_defaults(&config);
    char err[256] = "";

    /* A typo that silently does nothing is exactly what failing closed
       prevents. */
    EXPECT_FALSE(style_config_parse("{\"styl\": {}}", &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "unknown key"));
    EXPECT_NOT_NULL(strstr(err, "styl"));

    err[0] = '\0';
    EXPECT_FALSE(style_config_parse("{\"style\": {\"indent\": 4}}",
                                    &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "indent"));
}

MOLTEST(style_config_refuses_a_value_it_cannot_express) {
    style_config config;
    char err[256] = "";

    style_config_defaults(&config);
    EXPECT_FALSE(style_config_parse("{\"style\": {\"brace_style\": \"whitesmiths\"}}",
                                    &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "whitesmiths"));

    style_config_defaults(&config);
    err[0] = '\0';
    EXPECT_FALSE(style_config_parse("{\"style\": {\"indent_width\": \"four\"}}",
                                    &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "indent_width"));

    style_config_defaults(&config);
    err[0] = '\0';
    EXPECT_FALSE(style_config_parse("{\"style\": {\"use_tabs\": 1}}",
                                    &config, err, sizeof err));
}

MOLTEST(style_config_refuses_a_preset_it_has_not_implemented) {
    style_config config;
    char err[256] = "";

    /* RFC-0005 names kernel and gnu; saying they are not here yet beats
       formatting as something the user did not ask for. */
    style_config_defaults(&config);
    EXPECT_FALSE(style_config_parse("{\"preset\": \"kernel\"}", &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "not implemented"));

    style_config_defaults(&config);
    err[0] = '\0';
    EXPECT_FALSE(style_config_parse("{\"preset\": \"nope\"}", &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "unknown preset"));
}

MOLTEST(style_config_refuses_malformed_json) {
    style_config config;
    style_config_defaults(&config);
    char err[256] = "";

    EXPECT_FALSE(style_config_parse("{\"preset\":", &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "format.json"));

    err[0] = '\0';
    style_config_defaults(&config);
    EXPECT_FALSE(style_config_parse("[1,2]", &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "object"));
}

MOLTEST(lint_config_reads_a_severity_map) {
    lint_config config;
    lint_config_defaults(&config);
    char err[256] = "";

    ASSERT_TRUE(lint_config_parse(
        "{\n"
        "  \"backend\": \"clang-tidy@22.1.8\",\n"
        "  \"rules\": {\n"
        "    \"bugprone\": \"error\",\n"
        "    \"readability_magic_numbers\": \"warn\",\n"
        "    \"modernize\": \"off\"\n"
        "  }\n"
        "}\n", &config, err, sizeof err));

    ASSERT_EQ(3, (int)config.rule_count);
    EXPECT_STREQ("bugprone", config.rules[0].name);
    EXPECT_EQ(lint_severity_error, config.rules[0].severity);
    EXPECT_EQ(lint_severity_warn, config.rules[1].severity);
    EXPECT_EQ(lint_severity_off, config.rules[2].severity);
}

MOLTEST(lint_config_refuses_a_severity_that_is_not_off_warn_or_error) {
    lint_config config;
    lint_config_defaults(&config);
    char err[256] = "";

    EXPECT_FALSE(lint_config_parse("{\"rules\": {\"bugprone\": \"fatal\"}}",
                                   &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "fatal"));

    lint_config_defaults(&config);
    err[0] = '\0';
    EXPECT_FALSE(lint_config_parse("{\"rules\": {\"bugprone\": true}}",
                                   &config, err, sizeof err));
}

MOLTEST(lint_config_reports_too_many_rules_as_an_error) {
    char document[4096] = "{\"rules\": {";
    size_t used = strlen(document);
    for (int i = 0; i < LINT_MAX_RULES + 1; i++)
        used += (size_t)snprintf(document + used, sizeof document - used,
                                 "%s\"r%d\": \"warn\"", i > 0 ? "," : "", i);
    snprintf(document + used, sizeof document - used, "}}");

    lint_config config;
    lint_config_defaults(&config);
    char err[256] = "";
    /* Overflow is an error, never a silent truncation: the rules that did not
       fit are rules the project asked for. */
    EXPECT_FALSE(lint_config_parse(document, &config, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "too many"));
}

MOLTEST(style_excludes_match_a_directory_pattern) {
    style_excludes excludes;
    memset(&excludes, 0, sizeof excludes);
    snprintf(excludes.exclude[0], STYLE_EXCLUDE_MAX, "%s", "vendor/**");
    snprintf(excludes.exclude[1], STYLE_EXCLUDE_MAX, "%s", "*.generated.c");
    excludes.exclude_count = 2;

    EXPECT_TRUE(style_excludes_match(&excludes, "vendor/a/b.c"));
    EXPECT_TRUE(style_excludes_match(&excludes, "vendor"));
    EXPECT_TRUE(style_excludes_match(&excludes, "src/proto.generated.c"));

    /* A prefix is not a directory: "vendors" is a different tree. */
    EXPECT_FALSE(style_excludes_match(&excludes, "vendors/a.c"));
    EXPECT_FALSE(style_excludes_match(&excludes, "src/net.c"));
}

MOLTEST(style_config_reads_the_file_from_the_workspace_root) {
    char root[64];
    ASSERT_TRUE(workspace_setup(root, sizeof root));

    char path[128];
    snprintf(path, sizeof path, "%s/linter.json", root);
    FILE *file = fopen(path, "w");
    ASSERT_NOT_NULL(file);
    fputs("{\"rules\": {\"shadow\": \"error\"}}\n", file);
    fclose(file);

    lint_config config;
    char err[256] = "";
    ASSERT_TRUE(lint_config_load(root, &config, err, sizeof err));
    ASSERT_EQ(1, (int)config.rule_count);
    EXPECT_STREQ("shadow", config.rules[0].name);

    workspace_teardown(root);
}
