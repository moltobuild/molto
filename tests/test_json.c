#include <moltest.h>

#include <molto/util/json.h>

#include <stdio.h>
#include <string.h>

/* A configuration in the shape RFC-0005 specifies, which is what this parser
   exists to read. */
static const char linter_json[] =
    "{\n"
    "    \"backend\": \"clang-tidy@22.1.8\",\n"
    "    \"preset\": \"molto\",\n"
    "    \"exclude\": [\"vendor/**\", \"third_party/**\"],\n"
    "    \"rules\": {\n"
    "        \"bugprone\": \"error\",\n"
    "        \"readability_magic_numbers\": \"warn\",\n"
    "        \"modernize\": \"off\"\n"
    "    }\n"
    "}\n";

MOLTEST(json_reads_a_linter_configuration) {
    json_document *doc = json_parse(linter_json);
    ASSERT_NOT_NULL(doc);

    json_value root = json_root(doc);
    ASSERT_EQ(json_type_object, json_type_of(root));

    EXPECT_STREQ("clang-tidy@22.1.8", json_string(json_get(root, "backend")));
    EXPECT_STREQ("molto", json_string(json_get(root, "preset")));

    json_value exclude = json_get(root, "exclude");
    ASSERT_EQ(json_type_array, json_type_of(exclude));
    ASSERT_EQ(2, (int)json_count(exclude));
    EXPECT_STREQ("vendor/**", json_string(json_at(exclude, 0)));

    EXPECT_STREQ("error", json_string(json_get(json_get(root, "rules"), "bugprone")));

    json_free(doc);
}

MOLTEST(json_enumerates_the_members_of_an_object) {
    json_document *doc = json_parse(linter_json);
    ASSERT_NOT_NULL(doc);

    /* The severity map names rules Molto does not know in advance, so reading it
       means walking the members; refusing an unknown key needs the same walk. */
    json_value rules = json_get(json_root(doc), "rules");
    ASSERT_EQ(3, (int)json_count(rules));

    EXPECT_STREQ("bugprone", json_key_at(rules, 0));
    EXPECT_STREQ("error", json_string(json_member_at(rules, 0)));
    EXPECT_STREQ("readability_magic_numbers", json_key_at(rules, 1));
    EXPECT_STREQ("warn", json_string(json_member_at(rules, 1)));
    EXPECT_STREQ("modernize", json_key_at(rules, 2));
    EXPECT_STREQ("off", json_string(json_member_at(rules, 2)));

    json_free(doc);
}

MOLTEST(json_reports_no_members_for_what_is_not_an_object) {
    json_document *doc = json_parse("{\"a\":[1,2]}");
    ASSERT_NOT_NULL(doc);
    json_value root = json_root(doc);

    /* Past the end, and the wrong kind, are both answered rather than assumed. */
    EXPECT_NULL(json_key_at(root, 1));
    EXPECT_FALSE(json_is_valid(json_member_at(root, 1)));
    EXPECT_NULL(json_key_at(json_get(root, "a"), 0));
    EXPECT_FALSE(json_is_valid(json_member_at(json_get(root, "a"), 0)));
    EXPECT_NULL(json_key_at(json_get(root, "absent"), 0));

    json_free(doc);
}

MOLTEST(json_reports_every_scalar_type) {
    json_document *doc = json_parse(
        "{\"s\":\"text\",\"n\":-42,\"t\":true,\"f\":false,\"nothing\":null}");
    ASSERT_NOT_NULL(doc);
    json_value root = json_root(doc);

    EXPECT_EQ(json_type_string, json_type_of(json_get(root, "s")));
    EXPECT_EQ(json_type_number, json_type_of(json_get(root, "n")));
    EXPECT_EQ(json_type_bool, json_type_of(json_get(root, "t")));
    EXPECT_EQ(json_type_null, json_type_of(json_get(root, "nothing")));

    long long number = 0;
    EXPECT_TRUE(json_number(json_get(root, "n"), &number));
    EXPECT_EQ(-42, (int)number);

    bool flag = false;
    EXPECT_TRUE(json_bool(json_get(root, "t"), &flag));
    EXPECT_TRUE(flag);
    EXPECT_TRUE(json_bool(json_get(root, "f"), &flag));
    EXPECT_FALSE(flag);

    json_free(doc);
}

MOLTEST(json_decodes_escapes) {
    json_document *doc = json_parse("[\"a\\\"b\", \"c\\\\d\", \"e\\nf\", \"g\\u0041h\"]");
    ASSERT_NOT_NULL(doc);
    json_value root = json_root(doc);

    EXPECT_STREQ("a\"b", json_string(json_at(root, 0)));
    EXPECT_STREQ("c\\d", json_string(json_at(root, 1)));
    EXPECT_STREQ("e\nf", json_string(json_at(root, 2)));
    EXPECT_STREQ("gAh", json_string(json_at(root, 3)));

    json_free(doc);
}

MOLTEST(json_decodes_unicode_beyond_ascii) {
    /* Two bytes, three bytes, and a surrogate pair that must become one code
       point rather than two broken halves. */
    json_document *doc = json_parse("[\"\\u00f1\", \"\\u20ac\", \"\\ud83d\\ude00\"]");
    ASSERT_NOT_NULL(doc);
    json_value root = json_root(doc);

    EXPECT_STREQ("\xc3\xb1", json_string(json_at(root, 0)));         /* ñ */
    EXPECT_STREQ("\xe2\x82\xac", json_string(json_at(root, 1)));     /* € */
    EXPECT_STREQ("\xf0\x9f\x98\x80", json_string(json_at(root, 2))); /* emoji */

    json_free(doc);
}

MOLTEST(json_refuses_malformed_documents) {
    EXPECT_NULL(json_parse("{"));
    EXPECT_NULL(json_parse("{\"a\":}"));
    EXPECT_NULL(json_parse("[1,]"));
    EXPECT_NULL(json_parse("\"unterminated"));
    EXPECT_NULL(json_parse("{\"a\":1} trailing"));
    EXPECT_NULL(json_parse("tru"));
    EXPECT_NULL(json_parse("01"));
    EXPECT_NULL(json_parse(""));
    EXPECT_NULL(json_parse(NULL));
}

MOLTEST(json_refuses_a_document_nested_past_the_limit) {
    /* Deep enough to blow the stack if the parser recursed without a bound. */
    char deep[4 * JSON_MAX_DEPTH + 8];
    size_t at = 0;
    for (int i = 0; i < JSON_MAX_DEPTH + 2; i++)
        deep[at++] = '[';
    for (int i = 0; i < JSON_MAX_DEPTH + 2; i++)
        deep[at++] = ']';
    deep[at] = '\0';

    EXPECT_NULL(json_parse(deep));
}

MOLTEST(json_number_refuses_what_it_cannot_represent) {
    json_document *doc = json_parse("[1.5, 1e3, 99999999999999999999999]");
    ASSERT_NOT_NULL(doc);
    json_value root = json_root(doc);

    long long value = 0;
    /* Well-formed JSON numbers, but not integers this accessor can hand back;
       saying so is better than returning something rounded or saturated. */
    EXPECT_FALSE(json_number(json_at(root, 0), &value));
    EXPECT_FALSE(json_number(json_at(root, 1), &value));
    EXPECT_FALSE(json_number(json_at(root, 2), &value));

    json_free(doc);
}

MOLTEST(json_accessors_tolerate_what_is_not_there) {
    json_document *doc = json_parse("{\"a\":[1]}");
    ASSERT_NOT_NULL(doc);
    json_value root = json_root(doc);

    /* A key a configuration does not carry must not take the caller down. */
    json_value missing = json_get(root, "absent");
    EXPECT_FALSE(json_is_valid(missing));
    EXPECT_EQ(json_type_invalid, json_type_of(missing));
    EXPECT_NULL(json_string(missing));
    EXPECT_EQ(0, (int)json_count(missing));

    /* Chained lookups through a missing value stay invalid instead of faulting. */
    EXPECT_FALSE(json_is_valid(json_get(missing, "deeper")));
    EXPECT_FALSE(json_is_valid(json_at(missing, 0)));

    /* Asking for the wrong kind is answered, not assumed. */
    EXPECT_FALSE(json_is_valid(json_at(root, 0)));           /* object, not array */
    EXPECT_FALSE(json_is_valid(json_get(json_get(root, "a"), "x")));
    EXPECT_FALSE(json_is_valid(json_at(json_get(root, "a"), 5)));
    EXPECT_NULL(json_string(json_get(root, "a")));

    long long number = 0;
    EXPECT_FALSE(json_number(json_get(root, "a"), &number));
    bool flag = false;
    EXPECT_FALSE(json_bool(json_get(root, "a"), &flag));

    json_free(doc);
}

MOLTEST(json_handles_empty_containers_and_whitespace) {
    json_document *doc = json_parse("  {\n\t\"empty\": [],\n\t\"blank\": {}\n}  ");
    ASSERT_NOT_NULL(doc);
    json_value root = json_root(doc);

    EXPECT_EQ(json_type_array, json_type_of(json_get(root, "empty")));
    EXPECT_EQ(0, (int)json_count(json_get(root, "empty")));
    EXPECT_EQ(json_type_object, json_type_of(json_get(root, "blank")));
    EXPECT_EQ(2, (int)json_count(root));

    json_free(doc);
}

MOLTEST(json_free_accepts_null) {
    json_free(NULL); /* must not crash */
    EXPECT_TRUE(true);
}
