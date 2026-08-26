#include <moltest.h>

#include <molto/services/ir_service.h>
#include <molto/util/json.h>
#include <molto/util/toml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The IR document (RFC-0013): what it can say, what it refuses to say, and what
 * the engine refuses to lower from it.
 *
 * Three groups, and the middle one is the one that matters. The reader's job is
 * not to be permissive: an unknown attribute is ignored so a schema can grow,
 * and an unknown node type is fatal so a document is never built as something
 * other than what it says. Every refusal below is asserted for the message it
 * gives, because a rejection nobody can act on is a rejection that gets
 * worked around.
 */

/* --- helpers --- */

static size_t captured(FILE *file, char *out, size_t out_size) {
    (void)fflush(file);
    rewind(file);
    const size_t read = fread(out, 1, out_size - 1, file);
    out[read] = '\0';
    return read;
}

/* Write `doc` and hand back the bytes. */
static bool rendered(const ir_document *doc, char *out, size_t out_size) {
    FILE *scratch = tmpfile();
    if(scratch == NULL)
        return false;
    const bool ok = ir_write(doc, scratch);
    (void)captured(scratch, out, out_size);
    (void)fclose(scratch);
    return ok;
}

/* A document with one target, two sources and one dependency: enough shape to
   exercise every node this revision carries. */
static bool build_sample(ir_document *doc) {
    ir_document_init(doc);
    if(!ir_set_project(doc, "app", "0.1.0", "/w/app", IR_ORIGIN_NATIVE))
        return false;
    if(!str_list_push(&doc->files_read, "Project.toml"))
        return false;

    ir_target *target = ir_add_target(doc, "app", ir_target_executable);
    if(target == NULL)
        return false;

    ir_source *main_unit = ir_add_source(target, "src/main.c", ir_language_c);
    if(main_unit == NULL ||
       !ir_add_option(&main_unit->options, &main_unit->option_count, "-DMAIN", ir_scope_unit))
        return false;
    if(ir_add_source(target, "src/util.c", ir_language_c) == NULL)
        return false;

    if(!ir_add_option(&target->options, &target->option_count, "-DVERSION=1", ir_scope_target) ||
       !ir_add_include(&target->includes, &target->include_count, "include", ir_scope_target,
                       false) ||
       !ir_add_option(&target->links, &target->link_count, "m", ir_scope_target) ||
       !ir_set_artifact(target, ir_target_executable, "app", NULL))
        return false;

    ir_dependency *dep =
        ir_add_dependency(doc, "sqlite", "3.53.4", ir_dep_registry, ir_dep_scope_runtime,
                          "/w/app/.cache/sqlite");
    if(dep == NULL)
        return false;
    if(!ir_add_include(&dep->includes, &dep->include_count, "/w/app/.cache/sqlite",
                       ir_scope_target, true) ||
       !ir_add_option(&dep->links, &dep->link_count, "dl", ir_scope_target))
        return false;

    /* That dependency's own sources, as a target of their own. Its paths are
       relative to the package rather than to the project. */
    ir_target *objects = ir_add_target(doc, "sqlite:objects", ir_target_object);
    return objects != NULL && ir_set_target_package(objects, "sqlite") &&
           ir_add_source(objects, "sqlite3.c", ir_language_c) != NULL;
}

/* --- the model and the wire --- */

MOLTEST(ir_writes_a_document_that_reads_back_as_itself) {
    ir_document written;
    ASSERT_TRUE(build_sample(&written));

    char json[8192];
    ASSERT_TRUE(rendered(&written, json, sizeof json));

    ir_document read;
    char err[512] = "";
    ASSERT_TRUE(ir_read_json(json, &read, err, sizeof err));
    EXPECT_STREQ("", err);

    EXPECT_EQ(IR_SCHEMA, (int)read.schema);
    EXPECT_STREQ("app", read.name);
    EXPECT_STREQ("0.1.0", read.version);
    EXPECT_STREQ("/w/app", read.root);
    EXPECT_STREQ(IR_ORIGIN_NATIVE, read.origin);
    ASSERT_EQ(1u, str_list_count(&read.files_read));
    EXPECT_STREQ("Project.toml", str_list_get(&read.files_read, 0));

    ASSERT_EQ(2u, read.target_count);
    const ir_target *target = &read.targets[0];
    EXPECT_STREQ("app", target->name);
    EXPECT_EQ(ir_target_executable, target->kind);
    ASSERT_EQ(2u, target->source_count);
    EXPECT_STREQ("src/main.c", target->sources[0].path);
    EXPECT_EQ(ir_language_c, target->sources[0].language);
    ASSERT_EQ(1u, target->sources[0].option_count);
    EXPECT_STREQ("-DMAIN", target->sources[0].options[0].value);
    EXPECT_EQ(ir_scope_unit, target->sources[0].options[0].scope);
    ASSERT_EQ(1u, target->include_count);
    EXPECT_STREQ("include", target->includes[0].value);
    EXPECT_FALSE(target->includes[0].system);
    ASSERT_EQ(1u, target->link_count);
    EXPECT_STREQ("m", target->links[0].value);
    EXPECT_TRUE(target->has_artifact);
    EXPECT_STREQ("app", target->artifact.path);
    EXPECT_NULL(target->artifact.install);

    /* A package target comes back naming its package, and the project's own
       target comes back naming none — the two are different documents. */
    EXPECT_NULL(read.targets[0].package);
    EXPECT_STREQ("sqlite:objects", read.targets[1].name);
    EXPECT_STREQ("sqlite", read.targets[1].package);
    EXPECT_EQ(ir_target_object, read.targets[1].kind);
    ASSERT_EQ(1u, read.targets[1].source_count);
    EXPECT_STREQ("sqlite3.c", read.targets[1].sources[0].path);

    ASSERT_EQ(1u, read.dependency_count);
    const ir_dependency *dep = &read.dependencies[0];
    EXPECT_STREQ("sqlite", dep->name);
    EXPECT_STREQ("3.53.4", dep->version);
    EXPECT_EQ(ir_dep_registry, dep->origin);
    EXPECT_EQ(ir_dep_scope_runtime, dep->scope);
    ASSERT_EQ(1u, dep->include_count);
    EXPECT_TRUE(dep->includes[0].system);
    ASSERT_EQ(1u, dep->link_count);
    EXPECT_STREQ("dl", dep->links[0].value);

    ir_document_free(&read);
    ir_document_free(&written);
}

MOLTEST(ir_writes_the_same_bytes_twice) {
    /* `molto metadata`'s rule, and for the same reason: a dump that differs
       between runs cannot be diffed, and a document that cannot be diffed
       cannot be reviewed or cached (RFC-0013). */
    ir_document doc;
    ASSERT_TRUE(build_sample(&doc));

    char first[8192];
    char second[8192];
    ASSERT_TRUE(rendered(&doc, first, sizeof first));
    ASSERT_TRUE(rendered(&doc, second, sizeof second));
    EXPECT_STREQ(first, second);

    /* And nothing in it is a timestamp or a serial. */
    EXPECT_NULL(strstr(first, "timestamp"));
    EXPECT_NULL(strstr(first, "serialNumber"));

    ir_document_free(&doc);
}

MOLTEST(ir_omits_what_a_document_does_not_say) {
    /* What is not there and what is there and blank are different documents:
       an absent artifact and an absent install name are omitted, not written
       empty, so a reader can tell a producer that said nothing from one that
       said "nothing". */
    ir_document doc;
    ir_document_init(&doc);
    ASSERT_TRUE(ir_set_project(&doc, "bare", "0.1.0", "/w/bare", IR_ORIGIN_NATIVE));
    ASSERT_NOT_NULL(ir_add_target(&doc, "bare", ir_target_object));

    char json[4096];
    ASSERT_TRUE(rendered(&doc, json, sizeof json));
    EXPECT_NULL(strstr(json, "artifact"));
    EXPECT_NULL(strstr(json, "install"));
    /* The lists are still there, empty: their presence is the shape of the
       document and does not depend on what is in them. */
    EXPECT_NOT_NULL(strstr(json, "\"sources\": []"));

    ir_document_free(&doc);
}

MOLTEST(ir_omits_the_version_of_a_path_dependency) {
    /* Its bytes are whatever is on disk, so there is no version to state —
       the same omission `molto metadata` already makes. */
    ir_document doc;
    ir_document_init(&doc);
    ASSERT_TRUE(ir_set_project(&doc, "app", "0.1.0", "/w/app", IR_ORIGIN_NATIVE));
    ASSERT_NOT_NULL(
        ir_add_dependency(&doc, "http", NULL, ir_dep_path, ir_dep_scope_dev, "/w/app/modules/http"));

    char json[4096];
    ASSERT_TRUE(rendered(&doc, json, sizeof json));
    EXPECT_NOT_NULL(strstr(json, "\"origin\": \"path\""));

    /* One "version" in the document, and it is the project's own: the
       dependency states none. */
    size_t versions = 0;
    for(const char *at = json; (at = strstr(at, "\"version\"")) != NULL; at++)
        versions++;
    EXPECT_EQ(1u, versions);

    ir_document_free(&doc);
}

MOLTEST(ir_frees_a_document_twice_without_complaint) {
    ir_document doc;
    ASSERT_TRUE(build_sample(&doc));
    ir_document_free(&doc);
    ir_document_free(&doc);
    EXPECT_EQ(0u, doc.target_count);
    EXPECT_NULL(doc.name);
}

/* --- one document, two encodings --- */

/* The same project written as the TOML a fixture author types and the JSON a
   frontend returns. Both go through the same assertions, so a backend that
   starts reading nesting differently from the other fails here. */

static const char *const SAMPLE_TOML = "schema = 2\n"
                                       "files_read = [\"meson.build\"]\n"
                                       "[[projects]]\n"
                                       "name = \"app\"\n"
                                       "version = \"0.1.0\"\n"
                                       "root = \"/w/app\"\n"
                                       "origin = \"meson\"\n"
                                       "[[projects.targets]]\n"
                                       "name = \"app\"\n"
                                       "kind = \"executable\"\n"
                                       "[[projects.targets.sources]]\n"
                                       "path = \"src/main.c\"\n"
                                       "language = \"c\"\n"
                                       "[[projects.targets.includes]]\n"
                                       "value = \"include\"\n"
                                       "scope = \"target\"\n"
                                       "system = false\n"
                                       "[[projects.targets]]\n"
                                       "name = \"probe\"\n"
                                       "kind = \"object\"\n"
                                       "package = \"greet\"\n"
                                       "[[projects.targets.sources]]\n"
                                       "path = \"src/probe.cpp\"\n"
                                       "language = \"cpp\"\n";

static const char *const SAMPLE_JSON = "{\"schema\":2,\"files_read\":[\"meson.build\"],"
                                       "\"projects\":[{"
                                       "\"name\":\"app\",\"version\":\"0.1.0\","
                                       "\"root\":\"/w/app\",\"origin\":\"meson\","
                                       "\"targets\":["
                                       "{\"name\":\"app\",\"kind\":\"executable\","
                                       "\"sources\":[{\"path\":\"src/main.c\",\"language\":\"c\"}],"
                                       "\"includes\":[{\"value\":\"include\",\"scope\":\"target\","
                                       "\"system\":false}]},"
                                       "{\"name\":\"probe\",\"kind\":\"object\","
                                       "\"package\":\"greet\","
                                       "\"sources\":[{\"path\":\"src/probe.cpp\","
                                       "\"language\":\"cpp\"}]}]}]}";

static void assert_sample(const ir_document *doc) {
    EXPECT_STREQ("app", doc->name);
    EXPECT_STREQ("meson", doc->origin);
    EXPECT_TRUE(ir_is_from_plugin(doc));
    ASSERT_EQ(1u, str_list_count(&doc->files_read));
    EXPECT_STREQ("meson.build", str_list_get(&doc->files_read, 0));

    ASSERT_EQ(2u, doc->target_count);
    EXPECT_STREQ("app", doc->targets[0].name);
    ASSERT_EQ(1u, doc->targets[0].source_count);
    EXPECT_STREQ("src/main.c", doc->targets[0].sources[0].path);
    ASSERT_EQ(1u, doc->targets[0].include_count);
    EXPECT_STREQ("include", doc->targets[0].includes[0].value);

    /* The second target's sources are its own — the whole reason the reader
       needed a nested table accessor. */
    EXPECT_STREQ("probe", doc->targets[1].name);
    EXPECT_EQ(ir_target_object, doc->targets[1].kind);
    /* Whose sources these are, and therefore what its paths are relative to. */
    EXPECT_STREQ("greet", doc->targets[1].package);
    EXPECT_NULL(doc->targets[0].package);
    ASSERT_EQ(1u, doc->targets[1].source_count);
    EXPECT_STREQ("src/probe.cpp", doc->targets[1].sources[0].path);
    EXPECT_EQ(ir_language_cpp, doc->targets[1].sources[0].language);
    EXPECT_EQ(0u, doc->targets[1].include_count);
}

MOLTEST(ir_reads_the_same_document_from_toml) {
    char err[512] = "";
    toml_document *parsed = toml_parse(SAMPLE_TOML, err, sizeof err);
    ASSERT_NOT_NULL(parsed);

    ir_document doc;
    ASSERT_TRUE(ir_read(doc_from_toml(parsed), &doc, err, sizeof err));
    EXPECT_STREQ("", err);
    assert_sample(&doc);

    ir_document_free(&doc);
    toml_free(parsed);
}

MOLTEST(ir_reads_the_same_document_from_json) {
    ir_document doc;
    char err[512] = "";
    ASSERT_TRUE(ir_read_json(SAMPLE_JSON, &doc, err, sizeof err));
    EXPECT_STREQ("", err);
    assert_sample(&doc);
    ir_document_free(&doc);
}

/* --- what the reader refuses --- */

/* Read `json` expecting a refusal whose message contains `needle`. */
static void refused(const char *json, const char *needle) {
    ir_document doc;
    char err[512] = "";
    EXPECT_FALSE(ir_read_json(json, &doc, err, sizeof err));
    if(strstr(err, needle) == NULL) {
        char note[1024];
        snprintf(note, sizeof note, "expected a message mentioning '%s', got: %s", needle, err);
        FAIL(note);
    }
    /* A refused document is left empty, never half-read: valid JSON prefix and
       invalid meaning is worse than nothing. */
    EXPECT_EQ(0u, doc.target_count);
    EXPECT_NULL(doc.name);
    ir_document_free(&doc);
}

MOLTEST(ir_refuses_a_schema_it_does_not_speak) {
    refused("{\"schema\":3,\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
            "\"origin\":\"native\"}]}",
            "schema 3");
    /* A revision behind is refused just as loudly as one ahead, and that is
       what makes an added attribute safe: schema 1 has no `scope` on a
       dependency, so a reader that shrugged at the revision would read a
       development dependency as a runtime one and fold it into src/. */
    refused("{\"schema\":1,\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
            "\"origin\":\"native\"}]}",
            "schema 1");
    refused("{\"projects\":[]}", "no 'schema'");
}

MOLTEST(ir_refuses_an_unknown_node_type) {
    /* The directional rule of RFC-0013. An engine that skipped a node type it
       did not know would build something other than what it was handed, and
       report success — a green build of the wrong thing. */
    refused("{\"schema\":2,\"toolchains\":[{\"name\":\"gcc\"}],"
            "\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
            "\"origin\":\"native\"}]}",
            "'toolchains'");
}

MOLTEST(ir_refuses_a_build_step_by_name) {
    /* Refused by name rather than as an unknown key, because the node type
       exists in the specification and the reason it is absent is a reason a
       plugin author needs to read: it lowers to a command. */
    refused("{\"schema\":2,\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
            "\"origin\":\"meson\",\"steps\":[{\"name\":\"gen\",\"program\":\"sh\"}]}]}",
            "BuildStep");
}

MOLTEST(ir_refuses_a_generated_source_by_name) {
    /* The one case where "ignore what you don't know" points the wrong way: a
       GeneratedSource is a Source with two extra attributes, so ignoring them
       would compile a file nobody produced. */
    refused("{\"schema\":2,\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
            "\"origin\":\"meson\",\"targets\":[{\"name\":\"a\",\"kind\":\"executable\","
            "\"sources\":[{\"path\":\"src/gen.c\",\"language\":\"c\","
            "\"produced_by\":\"gen\",\"deterministic\":true}]}]}]}",
            "GeneratedSource");
}

MOLTEST(ir_refuses_a_vocabulary_it_does_not_know) {
    static const char *const PREFIX =
        "{\"schema\":2,\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
        "\"origin\":\"native\",";

    char json[1024];
    snprintf(json, sizeof json, "%s\"targets\":[{\"name\":\"a\",\"kind\":\"framework\"}]}]}",
             PREFIX);
    refused(json, "'framework'");

    snprintf(json, sizeof json,
             "%s\"targets\":[{\"name\":\"a\",\"kind\":\"executable\","
             "\"sources\":[{\"path\":\"a.rs\",\"language\":\"rust\"}]}]}]}",
             PREFIX);
    refused(json, "'rust'");

    snprintf(json, sizeof json,
             "%s\"targets\":[{\"name\":\"a\",\"kind\":\"executable\","
             "\"options\":[{\"value\":\"-DX\",\"scope\":\"everywhere\"}]}]}]}",
             PREFIX);
    refused(json, "'everywhere'");

    snprintf(json, sizeof json,
             "%s\"dependencies\":[{\"name\":\"d\",\"origin\":\"ftp\",\"scope\":\"runtime\","
             "\"root\":\"/w/d\"}]}]}",
             PREFIX);
    refused(json, "'ftp'");

    snprintf(json, sizeof json,
             "%s\"dependencies\":[{\"name\":\"d\",\"origin\":\"path\",\"scope\":\"build\","
             "\"root\":\"/w/d\"}]}]}",
             PREFIX);
    refused(json, "'build'");
}

MOLTEST(ir_refuses_a_dependency_that_does_not_say_who_may_use_it) {
    /* Not defaulted to `runtime`. A missing scope has two readings — "everything
       compiles against this" and "the producer did not say" — and picking the
       first hands a development dependency to src/, which is the one thing
       RFC-0008's separation exists to prevent. */
    refused("{\"schema\":2,\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
            "\"origin\":\"native\",\"dependencies\":[{\"name\":\"d\",\"origin\":\"path\","
            "\"root\":\"/w/d\"}]}]}",
            "missing a 'scope'");
}

MOLTEST(ir_refuses_a_document_that_is_not_exactly_one_project) {
    refused("{\"schema\":2,\"projects\":[]}", "exactly one");
    refused("{\"schema\":2,\"projects\":["
            "{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\",\"origin\":\"native\"},"
            "{\"name\":\"b\",\"version\":\"1\",\"root\":\"/w\",\"origin\":\"native\"}]}",
            "exactly one");
}

MOLTEST(ir_refuses_a_node_missing_what_it_is) {
    refused("{\"schema\":2,\"projects\":[{\"version\":\"1\",\"root\":\"/w\","
            "\"origin\":\"native\"}]}",
            "'name'");
    refused("{\"schema\":2,\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
            "\"origin\":\"native\",\"targets\":[{\"name\":\"t\"}]}]}",
            "'kind'");
    refused("{\"schema\":2,\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
            "\"origin\":\"native\",\"targets\":[{\"name\":\"t\",\"kind\":\"executable\","
            "\"sources\":[{\"path\":\"a.c\"}]}]}]}",
            "'language'");
}

MOLTEST(ir_ignores_an_attribute_it_does_not_know) {
    /* The other half of the rule, and the reason a schema can grow at all: an
       attribute refines work that is already described, so skipping one leaves
       the work described. */
    ir_document doc;
    char err[512] = "";
    ASSERT_TRUE(ir_read_json("{\"schema\":2,\"generator\":\"meson 1.4\","
                             "\"projects\":[{\"name\":\"a\",\"version\":\"1\",\"root\":\"/w\","
                             "\"origin\":\"native\",\"license\":\"MIT\","
                             "\"targets\":[{\"name\":\"t\",\"kind\":\"executable\","
                             "\"priority\":7,\"sources\":[{\"path\":\"a.c\","
                             "\"language\":\"c\",\"encoding\":\"utf-8\"}]}]}]}",
                             &doc, err, sizeof err));
    EXPECT_STREQ("", err);
    EXPECT_STREQ("a", doc.name);
    ASSERT_EQ(1u, doc.target_count);
    ASSERT_EQ(1u, doc.targets[0].source_count);
    EXPECT_STREQ("a.c", doc.targets[0].sources[0].path);
    ir_document_free(&doc);
}

/* --- what the engine refuses to lower --- */

static const ir_bounds BOUNDS = {
    .workspace = "/w/app",
    .build_dir = "/w/app/build/debug",
    .cache = "/home/u/.molto/cache",
};

/* Build a one-target document with `origin`, and hand back the target so a test
   can put the thing being refused on it. */
static ir_target *minimal(ir_document *doc, const char *origin) {
    ir_document_init(doc);
    if(!ir_set_project(doc, "app", "0.1.0", "/w/app", origin))
        return NULL;
    return ir_add_target(doc, "app", ir_target_executable);
}

static void rejects(const ir_document *doc, const char *needle) {
    char err[512] = "";
    EXPECT_FALSE(ir_validate(doc, &BOUNDS, err, sizeof err));
    if(strstr(err, needle) == NULL) {
        char note[1024];
        snprintf(note, sizeof note, "expected a message mentioning '%s', got: %s", needle, err);
        FAIL(note);
    }
}

MOLTEST(ir_validates_a_document_that_stays_inside_its_bounds) {
    ir_document doc;
    ir_target *target = minimal(&doc, IR_ORIGIN_NATIVE);
    ASSERT_NOT_NULL(target);
    ASSERT_NOT_NULL(ir_add_source(target, "src/main.c", ir_language_c));
    ASSERT_TRUE(ir_add_include(&target->includes, &target->include_count, "include",
                               ir_scope_target, false));
    ASSERT_TRUE(ir_set_artifact(target, ir_target_executable, "app", NULL));

    char err[512] = "";
    EXPECT_TRUE(ir_validate(&doc, &BOUNDS, err, sizeof err));
    EXPECT_STREQ("", err);
    ir_document_free(&doc);
}

MOLTEST(ir_refuses_a_path_that_climbs_out_of_the_workspace) {
    ir_document doc;
    ir_target *target = minimal(&doc, IR_ORIGIN_NATIVE);
    ASSERT_NOT_NULL(target);
    ASSERT_NOT_NULL(ir_add_source(target, "../../etc/shadow", ir_language_c));
    rejects(&doc, "outside the workspace");
    ir_document_free(&doc);
}

MOLTEST(ir_refuses_an_absolute_path_outside_the_workspace) {
    ir_document doc;
    ir_target *target = minimal(&doc, IR_ORIGIN_NATIVE);
    ASSERT_NOT_NULL(target);
    ASSERT_TRUE(ir_add_include(&target->includes, &target->include_count, "/etc", ir_scope_target,
                               false));
    rejects(&doc, "outside the workspace");
    ir_document_free(&doc);
}

MOLTEST(ir_allows_a_path_into_the_global_cache) {
    /* A dependency's sources live there, so the cache is a bound and not an
       exception to one. */
    ir_document doc;
    ir_document_init(&doc);
    ASSERT_TRUE(ir_set_project(&doc, "app", "0.1.0", "/w/app", IR_ORIGIN_NATIVE));
    ir_dependency *dep =
        ir_add_dependency(&doc, "sqlite", "3.53.4", ir_dep_registry, ir_dep_scope_runtime,
                          "/home/u/.molto/cache/sources/sqlite/3.53.4/any");
    ASSERT_NOT_NULL(dep);

    char err[512] = "";
    EXPECT_TRUE(ir_validate(&doc, &BOUNDS, err, sizeof err));
    EXPECT_STREQ("", err);
    ir_document_free(&doc);
}

MOLTEST(ir_anchors_a_package_target_at_the_dependency_root) {
    /* A dependency's bytes are in the shared cache, outside Project.root. The
       target names the package, so its sources stay relative — which is what
       keeps two machines producing the same document (RFC-0013). */
    ir_document doc;
    ir_document_init(&doc);
    ASSERT_TRUE(ir_set_project(&doc, "app", "0.1.0", "/w/app", IR_ORIGIN_NATIVE));
    ASSERT_NOT_NULL(ir_add_dependency(&doc, "sqlite", "3.53.4", ir_dep_registry,
                                      ir_dep_scope_runtime,
                                      "/home/u/.molto/cache/sources/sqlite/3.53.4/any"));

    ir_target *objects = ir_add_target(&doc, "sqlite:objects", ir_target_object);
    ASSERT_NOT_NULL(objects);
    ASSERT_TRUE(ir_set_target_package(objects, "sqlite"));
    /* Relative to the package, not to the project: anchored at Project.root
       this would resolve to /w/app/sqlite3.c and there is no such file. */
    ASSERT_NOT_NULL(ir_add_source(objects, "sqlite3.c", ir_language_c));
    ASSERT_TRUE(
        ir_add_include(&objects->includes, &objects->include_count, ".", ir_scope_target, false));

    char err[512] = "";
    EXPECT_TRUE(ir_validate(&doc, &BOUNDS, err, sizeof err));
    EXPECT_STREQ("", err);
    ir_document_free(&doc);
}

MOLTEST(ir_refuses_a_target_naming_a_package_the_document_does_not_describe) {
    /* Not a fallback to the project root. Falling back would anchor a
       dependency's sources somewhere they are not, and the document would look
       fine until a compile reported a missing file. */
    ir_document doc;
    ir_target *target = minimal(&doc, IR_ORIGIN_NATIVE);
    ASSERT_NOT_NULL(target);
    ASSERT_TRUE(ir_set_target_package(target, "ghost"));
    ASSERT_NOT_NULL(ir_add_source(target, "src/main.c", ir_language_c));
    rejects(&doc, "package 'ghost'");
    ir_document_free(&doc);
}

MOLTEST(ir_holds_a_package_target_to_the_same_bounds) {
    /* Naming a package moves the anchor, it does not lift the fence: a source
       that climbs out of the cache is refused exactly as one climbing out of
       the workspace is. */
    ir_document doc;
    ir_document_init(&doc);
    ASSERT_TRUE(ir_set_project(&doc, "app", "0.1.0", "/w/app", IR_ORIGIN_NATIVE));
    ASSERT_NOT_NULL(ir_add_dependency(&doc, "sqlite", "3.53.4", ir_dep_registry,
                                      ir_dep_scope_runtime,
                                      "/home/u/.molto/cache/sources/sqlite/3.53.4/any"));

    ir_target *objects = ir_add_target(&doc, "sqlite:objects", ir_target_object);
    ASSERT_NOT_NULL(objects);
    ASSERT_TRUE(ir_set_target_package(objects, "sqlite"));
    ASSERT_NOT_NULL(ir_add_source(objects, "../../../../../etc/shadow", ir_language_c));

    rejects(&doc, "outside the workspace");
    ir_document_free(&doc);
}

MOLTEST(ir_allows_a_path_under_a_root_the_caller_authorised) {
    /* The fourth bound. A `[deps]` entry of `{ path = "../greet" }` puts a
       sibling checkout on the compile line, and that directory is none of the
       three — it is authorised by the manifest the user wrote, which is what
       the caller passes in. */
    static const char *const ROOTS[] = {"/w/greet"};
    const ir_bounds bounds = {.workspace = "/w/app",
                              .build_dir = "/w/app/build/debug",
                              .cache = "/home/u/.molto/cache",
                              .roots = ROOTS,
                              .root_count = 1};

    ir_document doc;
    ir_target *target = minimal(&doc, IR_ORIGIN_NATIVE);
    ASSERT_NOT_NULL(target);
    ASSERT_TRUE(ir_add_include(&target->includes, &target->include_count, "/w/greet/include",
                               ir_scope_target, false));

    char err[512] = "";
    EXPECT_TRUE(ir_validate(&doc, &bounds, err, sizeof err));
    EXPECT_STREQ("", err);

    /* And the same document is refused without it, which is what makes the
       bound a bound rather than a formality. */
    EXPECT_FALSE(ir_validate(&doc, &BOUNDS, err, sizeof err));

    ir_document_free(&doc);
}

MOLTEST(ir_refuses_a_path_that_climbs_out_of_an_authorised_root) {
    /* An authorised root is a bound and not a hole: a recipe that names a
       directory above its own package — and a recipe is something a remote
       party wrote — is refused exactly as one climbing out of the workspace. */
    static const char *const ROOTS[] = {"/w/greet"};
    const ir_bounds bounds = {.workspace = "/w/app",
                              .build_dir = "/w/app/build/debug",
                              .cache = "/home/u/.molto/cache",
                              .roots = ROOTS,
                              .root_count = 1};

    ir_document doc;
    ir_target *target = minimal(&doc, IR_ORIGIN_NATIVE);
    ASSERT_NOT_NULL(target);
    ASSERT_TRUE(ir_add_include(&target->includes, &target->include_count, "/w/greet/../../etc",
                               ir_scope_target, false));

    char err[512] = "";
    EXPECT_FALSE(ir_validate(&doc, &bounds, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "outside the workspace"));

    ir_document_free(&doc);
}

MOLTEST(ir_does_not_mistake_a_sibling_directory_for_the_workspace) {
    /* Compared segment-wise: `/w/app-evil` shares a prefix with `/w/app` and is
       not inside it. A prefix test alone would let it through. */
    ir_document doc;
    ir_document_init(&doc);
    ASSERT_TRUE(ir_set_project(&doc, "app", "0.1.0", "/w/app-evil", IR_ORIGIN_NATIVE));
    rejects(&doc, "outside the workspace");
    ir_document_free(&doc);
}

MOLTEST(ir_refuses_an_artifact_outside_the_build_directory) {
    /* A target writing into src/ is editing the user's code as a side effect of
       a build. */
    ir_document doc;
    ir_target *target = minimal(&doc, IR_ORIGIN_NATIVE);
    ASSERT_NOT_NULL(target);
    ASSERT_TRUE(ir_set_artifact(target, ir_target_executable, "../../src/main.c", NULL));
    rejects(&doc, "outside the build directory");
    ir_document_free(&doc);
}

MOLTEST(ir_refuses_two_targets_with_one_name) {
    ir_document doc;
    ASSERT_NOT_NULL(minimal(&doc, IR_ORIGIN_NATIVE));
    ASSERT_NOT_NULL(ir_add_target(&doc, "app", ir_target_object));
    rejects(&doc, "two targets are named 'app'");
    ir_document_free(&doc);
}

MOLTEST(ir_refuses_a_dependency_on_a_target_that_is_not_there) {
    ir_document doc;
    ir_target *target = minimal(&doc, IR_ORIGIN_NATIVE);
    ASSERT_NOT_NULL(target);
    ASSERT_TRUE(str_list_push(&target->depends_on, "ghost"));
    rejects(&doc, "does not declare");
    ir_document_free(&doc);
}

MOLTEST(ir_refuses_a_cycle_before_the_scheduler_finds_it) {
    /* Reported against the document at the edge that closes it, never
       discovered as a deadlock (RFC-0013). */
    ir_document doc;
    ir_document_init(&doc);
    ASSERT_TRUE(ir_set_project(&doc, "app", "0.1.0", "/w/app", IR_ORIGIN_NATIVE));

    ir_target *first = ir_add_target(&doc, "a", ir_target_object);
    ASSERT_NOT_NULL(first);
    ASSERT_TRUE(str_list_push(&first->depends_on, "b"));
    ir_target *second = ir_add_target(&doc, "b", ir_target_object);
    ASSERT_NOT_NULL(second);
    ASSERT_TRUE(str_list_push(&second->depends_on, "a"));

    rejects(&doc, "depends on itself");
    ir_document_free(&doc);
}

/* --- the rules that apply only to a plugin's document --- */

/* One option, on a document from `origin`. */
static bool validate_with_option(const char *origin, const char *option, char *err,
                                 size_t err_size) {
    ir_document doc;
    ir_target *target = minimal(&doc, origin);
    if(target == NULL)
        return false;
    if(!ir_add_option(&target->options, &target->option_count, option, ir_scope_target)) {
        ir_document_free(&doc);
        return false;
    }
    const bool ok = ir_validate(&doc, &BOUNDS, err, err_size);
    ir_document_free(&doc);
    return ok;
}

MOLTEST(ir_refuses_a_plugin_option_that_loads_code_into_the_compiler) {
    /* A plugin denied the network, the filesystem and everything else can still
       return this. The sandbox decides what a plugin can touch; validation
       decides what molto will do on its behalf. */
    static const char *const REFUSED[] = {
        "-fplugin=/tmp/x.so",
        "-fplugin-arg-x-key=value",
        "-load",
        "-Xclang",
    };

    for(size_t i = 0; i < sizeof REFUSED / sizeof REFUSED[0]; i++) {
        char err[512] = "";
        char note[1024];
        if(validate_with_option("meson", REFUSED[i], err, sizeof err)) {
            snprintf(note, sizeof note, "'%s' should not be allowed from a plugin", REFUSED[i]);
            FAIL(note);
        } else if(strstr(err, "a plugin may not") == NULL) {
            snprintf(note, sizeof note, "'%s' was refused without saying why: %s", REFUSED[i], err);
            FAIL(note);
        }
    }
}

MOLTEST(ir_refuses_a_plugin_option_that_redirects_the_toolchain) {
    /* The toolchain is pickup's answer, not a frontend's opinion (RFC-0003). */
    static const char *const REFUSED[] = {
        "-B/tmp/bin",
        "--sysroot=/tmp/root",
        "-isysroot/tmp/root",
        "-fuse-ld=/tmp/ld",
    };

    for(size_t i = 0; i < sizeof REFUSED / sizeof REFUSED[0]; i++) {
        char err[512] = "";
        if(validate_with_option("meson", REFUSED[i], err, sizeof err)) {
            char note[1024];
            snprintf(note, sizeof note, "'%s' should not be allowed from a plugin", REFUSED[i]);
            FAIL(note);
        }
    }
}

MOLTEST(ir_refuses_a_plugin_naming_its_own_output) {
    /* The engine composes output paths; a producer naming one is describing
       where its object goes, which is not its decision. */
    char err[512] = "";
    EXPECT_FALSE(validate_with_option("meson", "-o", err, sizeof err));
    EXPECT_FALSE(validate_with_option("meson", "--output=/tmp/a.o", err, sizeof err));
}

MOLTEST(ir_holds_the_native_frontend_to_the_looser_rules) {
    /* The asymmetry is deliberate and it is not a statement about trust:
       Project.toml is a file in the user's repository, which their reviewer read
       and their version control records. `flags` is passed verbatim by contract
       (RFC-0007), and this is where that contract survives. */
    char err[512] = "";
    EXPECT_TRUE(validate_with_option(IR_ORIGIN_NATIVE, "-fplugin=/opt/mine.so", err, sizeof err));
    EXPECT_STREQ("", err);
    EXPECT_TRUE(validate_with_option(IR_ORIGIN_NATIVE, "--sysroot=/opt/sysroot", err, sizeof err));

    /* The path rules are not relaxed for it, though: those apply to every
       document whatever its origin. */
    ir_document doc;
    ir_target *target = minimal(&doc, IR_ORIGIN_NATIVE);
    ASSERT_NOT_NULL(target);
    ASSERT_NOT_NULL(ir_add_source(target, "/etc/passwd", ir_language_c));
    rejects(&doc, "outside the workspace");
    ir_document_free(&doc);
}

MOLTEST(ir_refuses_a_plugin_option_wherever_it_hides) {
    /* Every array a producer fills is checked, not only a target's own: a rule
       that covered the obvious one would be a rule with a way around it. */
    ir_document doc;
    ir_target *target = minimal(&doc, "meson");
    ASSERT_NOT_NULL(target);
    ir_source *source = ir_add_source(target, "src/main.c", ir_language_c);
    ASSERT_NOT_NULL(source);
    ASSERT_TRUE(ir_add_option(&source->options, &source->option_count, "-fplugin=/tmp/x.so",
                              ir_scope_unit));
    rejects(&doc, "a plugin may not");
    ir_document_free(&doc);

    ir_document with_dep;
    ir_document_init(&with_dep);
    ASSERT_TRUE(ir_set_project(&with_dep, "app", "0.1.0", "/w/app", "meson"));
    ir_dependency *dep =
        ir_add_dependency(&with_dep, "d", "1.0.0", ir_dep_registry, ir_dep_scope_runtime, "/w/app/d");
    ASSERT_NOT_NULL(dep);
    ASSERT_TRUE(ir_add_option(&dep->options, &dep->option_count, "-B/tmp", ir_scope_target));
    rejects(&with_dep, "a plugin may not");
    ir_document_free(&with_dep);
}
