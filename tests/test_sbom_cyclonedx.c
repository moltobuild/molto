#include <moltest.h>

#include <molto/build/sbom_cyclonedx.h>
#include <molto/services/sbom_service.h>
#include <molto/util/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Emitting CycloneDX 1.6.
 *
 * The document is built by hand here rather than resolved: every string in an
 * sbom_document is borrowed, so a literal is as good as a graph, and what is
 * under test is the spelling of the keys and nothing else. Whether the right
 * packages are in the document is test_sbom_service.c's question.
 *
 * Nearly every assertion goes through the repository's own JSON parser. A
 * writer checked against an expected string proves it wrote what the test
 * author imagined; one checked against a parser proves it wrote a document. */

/* A package with everything stated, one with nothing, and an edge between
   them. */
static void build_document(sbom_document *out, str_list *png_edges, str_list *zlib_edges,
                           manifest_about *about) {
    str_list_init(png_edges);
    (void)str_list_push(png_edges, "zlib");
    str_list_init(zlib_edges);

    memset(about, 0, sizeof *about);
    snprintf(about->description, sizeof about->description, "%s", "A demo application");
    snprintf(about->license, sizeof about->license, "%s", "MIT OR Apache-2.0");
    snprintf(about->repository, sizeof about->repository, "%s",
             "https://github.com/example/app");
    snprintf(about->authors[0], sizeof about->authors[0], "%s", "Ada");
    about->author_count = 1;

    static sbom_component components[2];
    components[0] = (sbom_component){
        .name = "png",
        .version = "1.6.40",
        .description = "The PNG reference library",
        .license = "libpng-2.0",
        .homepage = "http://www.libpng.org",
        .repository = "",
        .checksum = "8f14e45fceea167a5a36dedd4bea2543f14e45fceea167a5a36dedd4bea25438",
        .source = "registry+https://example.dev",
        .ships = true,
        .verified = true,
        .dependencies = png_edges,
    };
    components[1] = (sbom_component){
        .name = "zlib",
        .version = "",
        .description = "",
        .license = "",
        .homepage = "",
        .repository = "",
        .checksum = "",
        .source = "path+modules/zlib",
        .ships = false,
        .verified = false,
        .dependencies = zlib_edges,
    };

    memset(out, 0, sizeof *out);
    out->name = "app";
    out->version = "1.2.3";
    out->about = about;
    str_list_init(&out->root_dependencies);
    (void)str_list_push(&out->root_dependencies, "png");
    out->components = components;
    out->count = 2;
}

/* Emit into memory. Caller frees. */
static char *emit(const sbom_document *document) {
    char *text = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&text, &size);
    if (stream == NULL)
        return NULL;
    sbom_write_cyclonedx(stream, document, "9.9.9");
    fclose(stream);
    return text;
}

/* The component named `name` inside a parsed `components` array. */
static json_value component_named(json_value components, const char *name) {
    for (size_t i = 0; i < json_count(components); i++) {
        json_value candidate = json_at(components, i);
        const char *found = json_string(json_get(candidate, "name"));
        if (found != NULL && strcmp(found, name) == 0)
            return candidate;
    }
    return (json_value){0};
}

MOLTEST(the_bom_is_a_document_a_reader_will_take) {
    sbom_document document;
    str_list png_edges, zlib_edges;
    manifest_about about;
    build_document(&document, &png_edges, &zlib_edges, &about);

    char *text = emit(&document);
    ASSERT_NOT_NULL(text);
    json_document *parsed = json_parse(text);
    ASSERT_NOT_NULL(parsed);
    const json_value root = json_root(parsed);

    EXPECT_STREQ("CycloneDX", json_string(json_get(root, "bomFormat")));
    EXPECT_STREQ("1.6", json_string(json_get(root, "specVersion")));

    /* `version` is a number in this schema, not a string. */
    long long revision = 0;
    EXPECT_TRUE(json_number(json_get(root, "version"), &revision));
    EXPECT_EQ(1, (int)revision);

    json_free(parsed);
    free(text);
    str_list_free(&document.root_dependencies);
    str_list_free(&png_edges);
    str_list_free(&zlib_edges);
}

MOLTEST(the_bom_describes_the_package_and_the_tool) {
    sbom_document document;
    str_list png_edges, zlib_edges;
    manifest_about about;
    build_document(&document, &png_edges, &zlib_edges, &about);

    char *text = emit(&document);
    ASSERT_NOT_NULL(text);
    json_document *parsed = json_parse(text);
    ASSERT_NOT_NULL(parsed);

    const json_value metadata = json_get(json_root(parsed), "metadata");
    const json_value tool = json_at(json_get(json_get(metadata, "tools"), "components"), 0);
    EXPECT_STREQ("molto", json_string(json_get(tool, "name")));
    EXPECT_STREQ("9.9.9", json_string(json_get(tool, "version")));

    const json_value self = json_get(metadata, "component");
    EXPECT_STREQ("application", json_string(json_get(self, "type")));
    EXPECT_STREQ("app", json_string(json_get(self, "name")));
    EXPECT_STREQ("1.2.3", json_string(json_get(self, "version")));
    EXPECT_STREQ("A demo application", json_string(json_get(self, "description")));

    /* An SPDX *expression* is not an identifier, so it goes in `expression`. */
    const json_value license = json_at(json_get(self, "licenses"), 0);
    EXPECT_STREQ("MIT OR Apache-2.0", json_string(json_get(license, "expression")));

    const json_value author = json_at(json_get(self, "authors"), 0);
    EXPECT_STREQ("Ada", json_string(json_get(author, "name")));

    json_free(parsed);
    free(text);
    str_list_free(&document.root_dependencies);
    str_list_free(&png_edges);
    str_list_free(&zlib_edges);
}

MOLTEST(a_component_carries_its_licence_hash_and_origin) {
    sbom_document document;
    str_list png_edges, zlib_edges;
    manifest_about about;
    build_document(&document, &png_edges, &zlib_edges, &about);

    char *text = emit(&document);
    ASSERT_NOT_NULL(text);
    json_document *parsed = json_parse(text);
    ASSERT_NOT_NULL(parsed);

    const json_value components = json_get(json_root(parsed), "components");
    ASSERT_EQ(2u, json_count(components));

    const json_value png = component_named(components, "png");
    EXPECT_STREQ("library", json_string(json_get(png, "type")));
    EXPECT_STREQ("1.6.40", json_string(json_get(png, "version")));
    EXPECT_STREQ("required", json_string(json_get(png, "scope")));
    EXPECT_STREQ("pkg:generic/png@1.6.40", json_string(json_get(png, "purl")));
    EXPECT_STREQ("libpng-2.0",
                 json_string(json_get(json_at(json_get(png, "licenses"), 0), "expression")));

    const json_value hash = json_at(json_get(png, "hashes"), 0);
    EXPECT_STREQ("SHA-256", json_string(json_get(hash, "alg")));
    EXPECT_STREQ("8f14e45fceea167a5a36dedd4bea2543f14e45fceea167a5a36dedd4bea25438",
                 json_string(json_get(hash, "content")));

    const json_value reference = json_at(json_get(png, "externalReferences"), 0);
    EXPECT_STREQ("website", json_string(json_get(reference, "type")));
    EXPECT_STREQ("http://www.libpng.org", json_string(json_get(reference, "url")));

    json_free(parsed);
    free(text);
    str_list_free(&document.root_dependencies);
    str_list_free(&png_edges);
    str_list_free(&zlib_edges);
}

MOLTEST(what_was_not_stated_is_left_out_rather_than_written_empty) {
    /* A component whose recipe said nothing has no licence, no description and
       no hash — and an empty string in those fields would be a claim, not a
       silence. What it does carry is the property saying its bytes were never
       verified against anything. */
    sbom_document document;
    str_list png_edges, zlib_edges;
    manifest_about about;
    build_document(&document, &png_edges, &zlib_edges, &about);

    char *text = emit(&document);
    ASSERT_NOT_NULL(text);
    json_document *parsed = json_parse(text);
    ASSERT_NOT_NULL(parsed);

    const json_value zlib = component_named(json_get(json_root(parsed), "components"), "zlib");
    ASSERT_TRUE(json_is_valid(zlib));
    EXPECT_FALSE(json_is_valid(json_get(zlib, "licenses")));
    EXPECT_FALSE(json_is_valid(json_get(zlib, "description")));
    EXPECT_FALSE(json_is_valid(json_get(zlib, "hashes")));
    EXPECT_FALSE(json_is_valid(json_get(zlib, "version")));
    EXPECT_FALSE(json_is_valid(json_get(zlib, "externalReferences")));

    /* A dev dependency is in the graph and not in the artifact, which is what
       CycloneDX calls `excluded`. */
    EXPECT_STREQ("excluded", json_string(json_get(zlib, "scope")));
    EXPECT_STREQ("pkg:generic/zlib", json_string(json_get(zlib, "purl")));
    EXPECT_NOT_NULL(strstr(text, "molto:unverified"));

    json_free(parsed);
    free(text);
    str_list_free(&document.root_dependencies);
    str_list_free(&png_edges);
    str_list_free(&zlib_edges);
}

MOLTEST(the_bom_records_the_edges_including_the_roots) {
    sbom_document document;
    str_list png_edges, zlib_edges;
    manifest_about about;
    build_document(&document, &png_edges, &zlib_edges, &about);

    char *text = emit(&document);
    ASSERT_NOT_NULL(text);
    json_document *parsed = json_parse(text);
    ASSERT_NOT_NULL(parsed);

    const json_value dependencies = json_get(json_root(parsed), "dependencies");
    /* The root plus one entry per component: the graph, not a list. */
    ASSERT_EQ(3u, json_count(dependencies));

    const json_value root_edges = json_at(dependencies, 0);
    EXPECT_STREQ("app", json_string(json_get(root_edges, "ref")));
    EXPECT_STREQ("png", json_string(json_at(json_get(root_edges, "dependsOn"), 0)));

    const json_value png_entry = json_at(dependencies, 1);
    EXPECT_STREQ("png", json_string(json_get(png_entry, "ref")));
    EXPECT_STREQ("zlib", json_string(json_at(json_get(png_entry, "dependsOn"), 0)));

    /* A leaf still gets an entry, with an empty array. */
    const json_value zlib_entry = json_at(dependencies, 2);
    EXPECT_STREQ("zlib", json_string(json_get(zlib_entry, "ref")));
    EXPECT_EQ(0u, json_count(json_get(zlib_entry, "dependsOn")));

    json_free(parsed);
    free(text);
    str_list_free(&document.root_dependencies);
    str_list_free(&png_edges);
    str_list_free(&zlib_edges);
}

MOLTEST(the_bom_is_byte_for_byte_repeatable) {
    /* The reason there is no timestamp and no serial number in it. A document
       that differs between two runs over one graph cannot be diffed, cannot be
       cached, and cannot be compared between two machines — which is most of
       what a bill of materials is kept for. */
    sbom_document document;
    str_list png_edges, zlib_edges;
    manifest_about about;
    build_document(&document, &png_edges, &zlib_edges, &about);

    char *first = emit(&document);
    char *second = emit(&document);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    EXPECT_STREQ(first, second);

    EXPECT_NULL(strstr(first, "timestamp"));
    EXPECT_NULL(strstr(first, "serialNumber"));

    free(first);
    free(second);
    str_list_free(&document.root_dependencies);
    str_list_free(&png_edges);
    str_list_free(&zlib_edges);
}

MOLTEST(a_document_with_no_components_is_still_a_document) {
    manifest_about about;
    memset(&about, 0, sizeof about);

    sbom_document document;
    memset(&document, 0, sizeof document);
    document.name = "app";
    document.version = "0.1.0";
    document.about = &about;
    str_list_init(&document.root_dependencies);

    char *text = emit(&document);
    ASSERT_NOT_NULL(text);
    json_document *parsed = json_parse(text);
    ASSERT_NOT_NULL(parsed);

    const json_value root = json_root(parsed);
    EXPECT_EQ(0u, json_count(json_get(root, "components")));
    /* The root's own entry is there even with nothing under it. */
    ASSERT_EQ(1u, json_count(json_get(root, "dependencies")));
    EXPECT_NOT_NULL(strstr(text, "\"components\": []"));

    json_free(parsed);
    free(text);
    str_list_free(&document.root_dependencies);
}
