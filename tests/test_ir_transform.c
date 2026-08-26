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
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, NULL, err, sizeof err));
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

    /* A library reaches the document as the flag it already is, for the same
       reason a define does: a LinkOption is what reaches the link line. */
    ASSERT_EQ(1u, dep->link_count);
    EXPECT_STREQ("-lm", dep->links[0].value);

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
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, NULL, err, sizeof err));

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
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, NULL, err, sizeof err));

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
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, NULL, err, sizeof err));
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
    ASSERT_NOT_NULL(ir_add_dependency(&doc, "already", "1.0.0", ir_dep_git, ir_dep_scope_runtime,
                                      "/c/already"));

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_NOT_NULL(add_unit(&deps, "added", "2.0.0", dep_source_version, "/c/added"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, NULL, err, sizeof err));

    ASSERT_EQ(2u, doc.dependency_count);
    EXPECT_STREQ("already", doc.dependencies[0].name);
    EXPECT_STREQ("added", doc.dependencies[1].name);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

/* --- folding --- */

/* A document with an executable and a test target, as the native frontend
   produces for a project that has both. */
static void document_with_targets(ir_document *doc) {
    document_for(doc);
    ir_target *app = ir_add_target(doc, "app", ir_target_executable);
    ASSERT_NOT_NULL(app);
    ASSERT_TRUE(ir_add_include(&app->includes, &app->include_count, "src", ir_scope_target, false));
    ir_target *test = ir_add_target(doc, "tests/test_a", ir_target_test);
    ASSERT_NOT_NULL(test);
    ASSERT_TRUE(
        ir_add_include(&test->includes, &test->include_count, "src", ir_scope_target, false));
}

static bool carries_include(const ir_target *target, const char *value) {
    for(size_t i = 0; i < target->include_count; i++) {
        if(strcmp(target->includes[i].value, value) == 0)
            return true;
    }
    return false;
}

static bool carries_option(const ir_target *target, const char *value) {
    for(size_t i = 0; i < target->option_count; i++) {
        if(strcmp(target->options[i].value, value) == 0)
            return true;
    }
    return false;
}

MOLTEST(ir_transform_folds_a_runtime_dependency_into_every_target) {
    ir_document doc;
    document_with_targets(&doc);

    prepared_deps deps;
    prepared_deps_init(&deps);
    prepared_unit *unit = add_unit(&deps, "greet", "1.0.0", dep_source_path, "/w/app/modules/greet");
    ASSERT_NOT_NULL(unit);
    ASSERT_TRUE(str_list_push(&unit->exports.includes, "/w/app/modules/greet/include"));
    ASSERT_TRUE(str_list_push(&unit->exports.defines, "GREET_STATIC=1"));
    ASSERT_TRUE(str_list_push(&unit->exports.links, "m"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, NULL, err, sizeof err));
    ASSERT_TRUE(ir_transform_fold_dependencies(&doc, err, sizeof err));
    EXPECT_STREQ("", err);

    for(size_t i = 0; i < doc.target_count; i++) {
        const ir_target *target = &doc.targets[i];
        EXPECT_TRUE(carries_include(target, "/w/app/modules/greet/include"));
        EXPECT_TRUE(carries_option(target, "-DGREET_STATIC=1"));
        ASSERT_EQ(1u, target->link_count);
        EXPECT_STREQ("-lm", target->links[0].value);
        /* Appended after what the target already carried, which is where the
           build has always put them: include order decides which header wins. */
        EXPECT_STREQ("src", target->includes[0].value);
        EXPECT_STREQ("/w/app/modules/greet/include", target->includes[1].value);
    }

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

MOLTEST(ir_transform_keeps_a_development_dependency_out_of_the_executable) {
    /* The separation RFC-0008 calls enforcement rather than convention: a
       source under src/ that includes one fails to compile, on the first
       build, because the directory was never on its command line. */
    ir_document doc;
    document_with_targets(&doc);

    prepared_deps dev;
    prepared_deps_init(&dev);
    prepared_unit *unit = add_unit(&dev, "moltest", "", dep_source_path, "/w/app/modules/moltest");
    ASSERT_NOT_NULL(unit);
    ASSERT_TRUE(str_list_push(&unit->exports.includes, "/w/app/modules/moltest/include"));
    ASSERT_TRUE(str_list_push(&unit->exports.defines, "MOLTEST=1"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, NULL, &dev, err, sizeof err));
    ASSERT_TRUE(ir_transform_fold_dependencies(&doc, err, sizeof err));

    const ir_target *app = &doc.targets[0];
    EXPECT_EQ(ir_target_executable, app->kind);
    EXPECT_FALSE(carries_include(app, "/w/app/modules/moltest/include"));
    EXPECT_FALSE(carries_option(app, "-DMOLTEST=1"));

    const ir_target *test = &doc.targets[1];
    EXPECT_EQ(ir_target_test, test->kind);
    EXPECT_TRUE(carries_include(test, "/w/app/modules/moltest/include"));
    EXPECT_TRUE(carries_option(test, "-DMOLTEST=1"));

    ir_document_free(&doc);
    prepared_deps_free(&dev);
}

MOLTEST(ir_transform_folds_runtime_before_development) {
    /* A test target compiles against everything the project does and then some.
       The order is the one the build composes: what everything sees, then what
       only the tests do. */
    ir_document doc;
    document_with_targets(&doc);

    prepared_deps deps;
    prepared_deps deps_dev;
    prepared_deps_init(&deps);
    prepared_deps_init(&deps_dev);
    prepared_unit *runtime = add_unit(&deps, "greet", "1.0.0", dep_source_path, "/w/g");
    prepared_unit *devunit = add_unit(&deps_dev, "moltest", "", dep_source_path, "/w/m");
    ASSERT_NOT_NULL(runtime);
    ASSERT_NOT_NULL(devunit);
    ASSERT_TRUE(str_list_push(&runtime->exports.includes, "/w/g/include"));
    ASSERT_TRUE(str_list_push(&devunit->exports.includes, "/w/m/include"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, &deps_dev, err, sizeof err));
    ASSERT_TRUE(ir_transform_fold_dependencies(&doc, err, sizeof err));

    const ir_target *test = &doc.targets[1];
    ASSERT_EQ(3u, test->include_count);
    EXPECT_STREQ("src", test->includes[0].value);
    EXPECT_STREQ("/w/g/include", test->includes[1].value);
    EXPECT_STREQ("/w/m/include", test->includes[2].value);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
    prepared_deps_free(&deps_dev);
}

MOLTEST(ir_transform_says_which_scope_each_dependency_is) {
    /* The node says it, so the fold does not have to be told twice — and a
       consumer holding only the bytes can tell a development dependency from a
       runtime one, which is what makes the separation checkable off the
       document (RFC-0008). */
    ir_document doc;
    document_for(&doc);

    prepared_deps deps;
    prepared_deps dev;
    prepared_deps_init(&deps);
    prepared_deps_init(&dev);
    ASSERT_NOT_NULL(add_unit(&deps, "greet", "1.0.0", dep_source_path, "/w/g"));
    ASSERT_NOT_NULL(add_unit(&dev, "moltest", "", dep_source_path, "/w/m"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependencies(&doc, &deps, &dev, err, sizeof err));

    /* Runtime first and development second: walking the array once is what
       gives a test target its flags in the order the build composes them. */
    ASSERT_EQ(2u, doc.dependency_count);
    EXPECT_STREQ("greet", doc.dependencies[0].name);
    EXPECT_EQ(ir_dep_scope_runtime, doc.dependencies[0].scope);
    EXPECT_STREQ("moltest", doc.dependencies[1].name);
    EXPECT_EQ(ir_dep_scope_dev, doc.dependencies[1].scope);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
    prepared_deps_free(&dev);
}

MOLTEST(ir_transform_folds_from_the_document_alone) {
    /* The fold is handed a document and nothing else. A consumer that has only
       the published bytes — no `prepared_deps`, no resolve — folds them exactly
       as the engine does, which is what a transform of RFC-0015 is for. */
    ir_document doc;
    document_with_targets(&doc);

    ir_dependency *runtime =
        ir_add_dependency(&doc, "greet", "1.0.0", ir_dep_path, ir_dep_scope_runtime, "/w/g");
    ir_dependency *devdep =
        ir_add_dependency(&doc, "moltest", NULL, ir_dep_path, ir_dep_scope_dev, "/w/m");
    ASSERT_NOT_NULL(runtime);
    ASSERT_NOT_NULL(devdep);
    ASSERT_TRUE(ir_add_include(&runtime->includes, &runtime->include_count, "/w/g/include",
                               ir_scope_target, false));
    ASSERT_TRUE(ir_add_include(&devdep->includes, &devdep->include_count, "/w/m/include",
                               ir_scope_target, false));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_fold_dependencies(&doc, err, sizeof err));
    EXPECT_STREQ("", err);

    const ir_target *app = &doc.targets[0];
    EXPECT_TRUE(carries_include(app, "/w/g/include"));
    EXPECT_FALSE(carries_include(app, "/w/m/include"));

    const ir_target *test = &doc.targets[1];
    EXPECT_TRUE(carries_include(test, "/w/g/include"));
    EXPECT_TRUE(carries_include(test, "/w/m/include"));

    ir_document_free(&doc);
}

/* --- a dependency's own sources as targets --- */

/* Give a unit sources under its root, the way `resolve` leaves them. */
static bool add_source(prepared_unit *unit, const char *relative) {
    char absolute[512];
    snprintf(absolute, sizeof absolute, "%s/%s", unit->root, relative);
    return str_list_push(&unit->sources, absolute);
}

static const ir_target *target_named(const ir_document *doc, const char *name) {
    for(size_t i = 0; i < doc->target_count; i++) {
        if(strcmp(doc->targets[i].name, name) == 0)
            return &doc->targets[i];
    }
    return NULL;
}

MOLTEST(ir_transform_describes_a_package_relative_to_its_own_root) {
    /* A package's bytes are in the shared cache. Writing them absolute would
       put one machine's home directory in a document two machines are supposed
       to be able to diff. */
    ir_document doc;
    document_for(&doc);

    prepared_deps deps;
    prepared_deps_init(&deps);
    prepared_unit *unit =
        add_unit(&deps, "yyjson", "0.10.0", dep_source_version, "/home/u/.molto/cache/yyjson");
    ASSERT_NOT_NULL(unit);
    ASSERT_TRUE(add_source(unit, "src/yyjson.c"));
    ASSERT_TRUE(str_list_push(&unit->includes, "/home/u/.molto/cache/yyjson/include"));
    ASSERT_TRUE(str_list_push(&unit->defines, "YYJSON_STATIC=1"));
    ASSERT_TRUE(str_list_push(&unit->flags, "-fno-strict-aliasing"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependency_targets(&doc, &deps, NULL, "c17", "", err, sizeof err));
    EXPECT_STREQ("", err);

    const ir_target *target = target_named(&doc, "yyjson:objects");
    ASSERT_NOT_NULL(target);
    EXPECT_EQ(ir_target_object, target->kind);
    EXPECT_STREQ("yyjson", target->package);

    ASSERT_EQ(1u, target->source_count);
    EXPECT_STREQ("src/yyjson.c", target->sources[0].path);
    EXPECT_EQ(ir_language_c, target->sources[0].language);

    /* Its own recipe, in the order a command line receives it. */
    ASSERT_EQ(2u, target->option_count);
    EXPECT_STREQ("-DYYJSON_STATIC=1", target->options[0].value);
    EXPECT_STREQ("-fno-strict-aliasing", target->options[1].value);
    ASSERT_EQ(1u, target->include_count);
    EXPECT_STREQ("/home/u/.molto/cache/yyjson/include", target->includes[0].value);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

MOLTEST(ir_transform_gives_a_package_its_own_standard) {
    /* A recipe that names one wins; a recipe that names none falls back to the
       consumer's, which is what every package did before recipes could say. */
    ir_document doc;
    document_for(&doc);

    prepared_deps deps;
    prepared_deps_init(&deps);
    /* Both packages first, then the pointers: `add_unit` grows the array, so one
       taken before the second call is left dangling by it. */
    ASSERT_NOT_NULL(add_unit(&deps, "own", "1.0.0", dep_source_version, "/c/own"));
    ASSERT_NOT_NULL(add_unit(&deps, "silent", "1.0.0", dep_source_version, "/c/silent"));
    prepared_unit *own = &deps.units[0];
    prepared_unit *silent = &deps.units[1];
    snprintf(own->std, sizeof own->std, "%s", "c99");
    ASSERT_TRUE(add_source(own, "a.c"));
    ASSERT_TRUE(add_source(silent, "b.c"));
    ASSERT_TRUE(add_source(silent, "c.cpp"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependency_targets(&doc, &deps, NULL, "c17", "c++20", err,
                                                sizeof err));

    const ir_target *named = target_named(&doc, "own:objects");
    ASSERT_NOT_NULL(named);
    ASSERT_EQ(1u, named->sources[0].option_count);
    EXPECT_STREQ("-std=c99", named->sources[0].options[0].value);
    /* Unit scope, so it is the last thing the compiler sees about that file. */
    EXPECT_EQ(ir_scope_unit, named->sources[0].options[0].scope);

    const ir_target *fell_back = target_named(&doc, "silent:objects");
    ASSERT_NOT_NULL(fell_back);
    ASSERT_EQ(2u, fell_back->source_count);
    /* Per language, not per package: the .c and the .cpp are different files. */
    EXPECT_STREQ("-std=c17", fell_back->sources[0].options[0].value);
    EXPECT_EQ(ir_language_cpp, fell_back->sources[1].language);
    EXPECT_STREQ("-std=c++20", fell_back->sources[1].options[0].value);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

MOLTEST(ir_transform_leaves_a_header_only_package_without_a_target) {
    /* There is nothing to compile. An empty target would be a node the engine
       has to know to skip rather than one it never had. */
    ir_document doc;
    document_for(&doc);

    prepared_deps deps;
    prepared_deps_init(&deps);
    ASSERT_NOT_NULL(add_unit(&deps, "headers", "1.0.0", dep_source_version, "/c/headers"));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_dependency_targets(&doc, &deps, NULL, "", "", err, sizeof err));
    EXPECT_EQ(0u, doc.target_count);

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

MOLTEST(ir_transform_refuses_a_source_outside_its_package) {
    /* Writing it absolute would be the one thing this transform exists to
       avoid, so it says so instead of doing it. */
    ir_document doc;
    document_for(&doc);

    prepared_deps deps;
    prepared_deps_init(&deps);
    prepared_unit *unit = add_unit(&deps, "odd", "1.0.0", dep_source_version, "/c/odd");
    ASSERT_NOT_NULL(unit);
    ASSERT_TRUE(str_list_push(&unit->sources, "/elsewhere/stray.c"));

    char err[512] = "";
    EXPECT_FALSE(ir_transform_dependency_targets(&doc, &deps, NULL, "", "", err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "not under its root"));

    ir_document_free(&doc);
    prepared_deps_free(&deps);
}

MOLTEST(ir_transform_keeps_the_fold_out_of_a_package) {
    /* A package is compiled against its own recipe and nothing the consumer
       resolved — not another dependency's headers, and not its own. It is what
       makes one package compile identically everywhere, which is what lets an
       object be shared between projects. */
    ir_document doc;
    document_for(&doc);

    ir_target *objects = ir_add_target(&doc, "greet:objects", ir_target_object);
    ASSERT_NOT_NULL(objects);
    ASSERT_TRUE(ir_set_target_package(objects, "greet"));

    ir_dependency *other =
        ir_add_dependency(&doc, "other", "1.0.0", ir_dep_registry, ir_dep_scope_runtime, "/c/other");
    ASSERT_NOT_NULL(other);
    ASSERT_TRUE(ir_add_include(&other->includes, &other->include_count, "/c/other/include",
                               ir_scope_target, false));

    char err[512] = "";
    ASSERT_TRUE(ir_transform_fold_dependencies(&doc, err, sizeof err));

    EXPECT_EQ(0u, objects->include_count);
    EXPECT_EQ(0u, objects->option_count);

    ir_document_free(&doc);
}
