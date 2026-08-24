#include <moltest.h>

#include <molto/services/ir_transform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A transform is a function from a document to a document, and that is exactly
 * how it is tested: one built by hand goes in, and what comes out is compared
 * node by node. No build runs, nothing is resolved and nothing is fetched —
 * which is the argument RFC-0015 makes for transforms over hooks, whose test
 * would be a build and an assertion about a side effect.
 */

/* A package as `resolve` leaves it: what it is, where its bytes are, and the
   interface it exports. */
static prepared_unit *add_unit(prepared_deps *deps, const char *name, const char *version,
                               dep_source origin, const char *root) {
    prepared_unit *grown = realloc(deps->units, (deps->unit_count + 1) * sizeof *grown);
    if(grown == NULL)
        return NULL;
    deps->units = grown;
    prepared_unit *unit = &deps->units[deps->unit_count++];
    memset(unit, 0, sizeof *unit);
    snprintf(unit->name, sizeof unit->name, "%s", name);
    snprintf(unit->version, sizeof unit->version, "%s", version);
    unit->origin = origin;
    snprintf(unit->root, sizeof unit->root, "%s", root);
    str_list_init(&unit->sources);
    str_list_init(&unit->includes);
    str_list_init(&unit->defines);
    str_list_init(&unit->flags);
    str_list_init(&unit->exports.includes);
    str_list_init(&unit->exports.defines);
    str_list_init(&unit->exports.flags);
    str_list_init(&unit->exports.links);
    return unit;
}

static void document_for(ir_document *doc) {
    ir_document_init(doc);
    ASSERT_TRUE(ir_set_project(doc, "app", "1.0.0", "/w/app", IR_ORIGIN_NATIVE));
}

MOLTEST(ir_transform_says_what_each_dependency_exports) {
    ir_document doc;
    document_for(&doc);

    prepared_deps deps;
    prepared_deps_init(&deps);
    prepared_unit *unit = add_unit(&deps, "yyjson", "0.10.0", dep_source_version,
                                   "/home/u/.molto/cache/sources/yyjson/0.10.0/any");
    ASSERT_NOT_NULL(unit);
    ASSERT_TRUE(str_list_push(&unit->exports.includes,
                              "/home/u/.molto/cache/sources/yyjson/0.10.0/any/include"));
    ASSERT_TRUE(str_list_push(&unit->exports.defines, "YYJSON_STATIC=1"));
    ASSERT_TRUE(str_list_push(&unit->exports.flags, "-fno-strict-aliasing"));
    ASSERT_TRUE(str_list_push(&unit->exports.links, "m"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, err, sizeof err));
    EXPECT_STREQ("", err);

    ASSERT_EQ(1u, doc.dependency_count);
    const ir_dependency *dep = &doc.dependencies[0];
    EXPECT_STREQ("yyjson", dep->name);
    EXPECT_STREQ("0.10.0", dep->version);
    EXPECT_EQ(ir_dep_registry, dep->origin);
    EXPECT_STREQ("/home/u/.molto/cache/sources/yyjson/0.10.0/any", dep->root);

    ASSERT_EQ(1u, dep->include_count);
    EXPECT_STREQ("/home/u/.molto/cache/sources/yyjson/0.10.0/any/include",
                 dep->includes[0].value);
    /* Never -isystem: that suppresses warnings in the headers it names, and no
       Molto build has ever silenced a dependency's diagnostics. */
    EXPECT_FALSE(dep->includes[0].system);
    EXPECT_EQ(ir_scope_target, dep->includes[0].scope);

    /* A define reaches the document as the flag it already is: the document
       carries options as they reach a command line, so a consumer can read one
       without knowing which table it came from. */
    ASSERT_EQ(2u, dep->option_count);
    EXPECT_STREQ("-DYYJSON_STATIC=1", dep->options[0].value);
    EXPECT_STREQ("-fno-strict-aliasing", dep->options[1].value);

    ASSERT_EQ(1u, dep->link_count);
    EXPECT_STREQ("m", dep->links[0].value);

    /* The transform touched nothing else. */
    EXPECT_STREQ("app", doc.name);
    EXPECT_EQ(0u, doc.target_count);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

MOLTEST(ir_transform_gives_a_path_dependency_no_version) {
    /* Its bytes are whatever is on disk, so the node says so by having no
       version rather than by inventing one. */
    ir_document doc;
    document_for(&doc);

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_NOT_NULL(add_unit(&deps, "http", "", dep_source_path, "/w/app/modules/http"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, err, sizeof err));

    ASSERT_EQ(1u, doc.dependency_count);
    EXPECT_STREQ("http", doc.dependencies[0].name);
    EXPECT_NULL(doc.dependencies[0].version);
    EXPECT_EQ(ir_dep_path, doc.dependencies[0].origin);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

MOLTEST(ir_transform_spells_every_origin_the_document_has) {
    /* Two enumerations for one idea, translated in one place. A cast would
       compile and would be wrong the day either grows a value. */
    ir_document doc;
    document_for(&doc);

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_NOT_NULL(add_unit(&deps, "a", "1.0.0", dep_source_version, "/c/a"));
    ASSERT_NOT_NULL(add_unit(&deps, "b", "", dep_source_git, "/c/b"));
    ASSERT_NOT_NULL(add_unit(&deps, "c", "", dep_source_path, "/c/c"));
    ASSERT_NOT_NULL(add_unit(&deps, "d", "2.0.0", dep_source_archive, "/c/d"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, err, sizeof err));

    ASSERT_EQ(4u, doc.dependency_count);
    EXPECT_EQ(ir_dep_registry, doc.dependencies[0].origin);
    EXPECT_EQ(ir_dep_git, doc.dependencies[1].origin);
    EXPECT_EQ(ir_dep_path, doc.dependencies[2].origin);
    EXPECT_EQ(ir_dep_archive, doc.dependencies[3].origin);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

MOLTEST(ir_transform_says_nothing_when_there_is_nothing_to_say) {
    /* A project with no dependencies gets no nodes, not empty ones. */
    ir_document doc;
    document_for(&doc);

    prepared_deps deps;
    prepared_deps_init(&deps);

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, err, sizeof err));
    EXPECT_EQ(0u, doc.dependency_count);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

MOLTEST(ir_transform_keeps_what_the_document_already_named) {
    /* It adds and never replaces. A transform that silently dropped what an
       earlier one wrote would be a composition rule nobody could reason
       about — and composition is the whole argument for transforms. */
    ir_document doc;
    document_for(&doc);
    ASSERT_NOT_NULL(ir_add_dependency(&doc, "already", "1.0.0", ir_dep_git, "/c/already"));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_NOT_NULL(add_unit(&deps, "added", "2.0.0", dep_source_version, "/c/added"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, err, sizeof err));

    ASSERT_EQ(2u, doc.dependency_count);
    EXPECT_STREQ("already", doc.dependencies[0].name);
    EXPECT_STREQ("added", doc.dependencies[1].name);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}
