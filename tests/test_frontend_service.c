#include <moltest.h>

#include <molto/services/frontend_service.h>
#include <molto/util/thread.h>
#include <molto/services/fs_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The `frontend` capability, end to end.
 *
 * Every plugin below is a shell script, and that is the point: the contract is a
 * process protocol, not an ABI, so anything that can read standard input and
 * write standard output is a plugin. A test that needed a compiled plugin to
 * exercise the contract would be testing something narrower than the contract.
 *
 * The sandbox gives each test its own HOME, so what is "installed" is what the
 * test planted rather than whatever the machine running it happens to have.
 */

typedef struct {
    char root[64];
    char home[128];
    char installed[192]; /* ~/.molto/plugins/bin */
    char recipes[192];   /* ~/.molto/plugins/recipes */
    char project[192];   /* the directory being described */
    char old_home[4096];
} sandbox;

static bool sandbox_setup(sandbox *box) {
    if(!moltest_temp_dir("molto_frontend", box->root, sizeof box->root))
        return false;

    snprintf(box->home, sizeof box->home, "%s/home", box->root);
    snprintf(box->installed, sizeof box->installed, "%s/.molto/plugins/bin", box->home);
    snprintf(box->recipes, sizeof box->recipes, "%s/.molto/plugins/recipes", box->home);
    snprintf(box->project, sizeof box->project, "%s/project", box->root);

    const char *home = getenv("HOME");
    snprintf(box->old_home, sizeof box->old_home, "%s", home != NULL ? home : "");

    return fs_make_dirs(box->installed) && fs_make_dirs(box->recipes) &&
           fs_make_dirs(box->project) && setenv("HOME", box->home, 1) == 0;
}

static void sandbox_teardown(sandbox *box) {
    (void)setenv("HOME", box->old_home, 1);
    char command[128];
    (void)fs_remove_tree(box->root);
}

/*
 * The plugin a frontend test installs: it drains its request the way the
 * contract asks, prints the document it was given, and leaves with the status
 * it was told to.
 *
 * A real program rather than a `#!/bin/sh` file. A shebang is honoured by the
 * kernel and means nothing to CreateProcess, so every script here was a plugin
 * Windows could not start -- and a frontend that cannot start is indomitable
 * from a frontend that answered wrongly, which is what these tests are for.
 */
MOLTEST_FAKE(fake_frontend) {
    /* Read to EOF first, which is what the contract asks of a real one: the
       caller is writing a request and blocks until somebody takes it. */
    (void)moltest_fake_input();

    const char *log = moltest_fake_setting("log");
    if(log != NULL) {
        FILE *file = fopen(log, "wb");
        if(file != NULL) {
            fprintf(file, "argv1=%s\n", argc > 1 ? argv[1] : "");
            fputs(moltest_fake_input(), file);
            (void)fclose(file);
        }
    }

    const char *hang = moltest_fake_setting("sleep_ms");
    if(hang != NULL)
        thread_sleep_ms((unsigned)atoi(hang));

    const char *document = moltest_fake_setting("document");
    if(document != NULL) {
        char *text = fs_read_file(document);
        if(text != NULL) {
            printf("%s", text);
            free(text);
        }
    }

    const char *code = moltest_fake_setting("exit");
    return code != NULL ? atoi(code) : 0;
}

/* Append one line to a spec, refusing rather than truncating. */
[[gnu::format(printf, 4, 5)]]
static bool spec_append(char *out, size_t size, size_t *used, const char *format, ...) {
    if(*used >= size)
        return false;

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out + *used, size - *used, format, args);
    va_end(args);

    if(written < 0 || (size_t)written >= size - *used)
        return false;
    *used += (size_t)written;
    return true;
}

/* Compose what that stub should do. The document travels in a file because a
   spec line is one line and a document is many. */
static bool frontend_spec(const sandbox *box, const char *document, int code, unsigned sleep_ms,
                          const char *log, char *out, size_t size) {
    static unsigned nth = 0;
    char document_path[320] = "";
    if(document != NULL) {
        snprintf(document_path, sizeof document_path, "%s/document-%u", box->root, nth++);
        if(!fs_write_file(document_path, document))
            return false;
    }

    /* Every line is appended through here, and a line that does not fit stops
       the whole thing. `snprintf` reports what it *would* have written, so
       adding its return to an offset walks past the end of the buffer the
       moment one truncates -- and the next call is then handed a pointer
       outside the array and a length that underflowed to something enormous.
       Windows found this before Linux did: its temporary paths are long enough
       to truncate a spec that fits comfortably under /tmp. */
    size_t used = 0;
    if(!spec_append(out, size, &used, "set exit %d\n", code))
        return false;
    if(document != NULL && !spec_append(out, size, &used, "set document %s\n", document_path))
        return false;
    if(sleep_ms > 0 && !spec_append(out, size, &used, "set sleep_ms %u\n", sleep_ms))
        return false;
    if(log != NULL && !spec_append(out, size, &used, "set log %s\n", log))
        return false;
    return spec_append(out, size, &used, "behave fake_frontend\n");
}

/* Install a plugin: the executable, and the recipe beside it that says what it
   may do. Both are needed — a binary with no recipe is not a candidate, because
   nothing recorded what it asked for. */
static bool install(const sandbox *box, const char *name, const char *spec,
                    const char *recipe) {
    char path[320];
    snprintf(path, sizeof path, "%s/molto-%s", box->installed, name);
    if(!moltest_fake_program(path, spec, NULL, 0))
        return false;

    char recipe_path[320];
    snprintf(recipe_path, sizeof recipe_path, "%s/%s.toml", box->recipes, name);
    return fs_write_file(recipe_path, recipe);
}

/* A recipe for a frontend plugin: the four things RFC-0014 says one declares. */
static void frontend_recipe(char *out, size_t size, const char *name, const char *extension,
                            long ir_schema, const char *molto_min) {
    snprintf(out, size,
             "schema = 2\n"
             "form = \"binary\"\n"
             "kind = \"tool\"\n"
             "name = \"%s\"\n"
             "version = \"0.1.0\"\n"
             "target = \"x86_64-unknown-linux-gnu\"\n"
             "[tool]\n"
             "kind = \"plugin\"\n"
             "[plugin]\n"
             "capabilities = [\"frontend\"]\n"
             "extensions = [\"%s\"]\n"
             "permissions = [\"ir.write\", \"project.read\"]\n"
             "ir_schema = %ld\n"
             "molto_min = \"%s\"\n",
             name, extension, ir_schema, molto_min);
}

/* A script that answers with a valid document naming `origin`. It reads its
   request to EOF first, which is what the contract asks of a real one. */
static void answering_script(char *out, size_t size, const char *origin) {
    snprintf(out, size,
             "{\"schema\":3,\"files_read\":[\"meson.build\"],\"projects\":[{"
             "\"name\":\"app\",\"version\":\"0.1.0\",\"root\":\"%%ROOT%%\","
             "\"origin\":\"%s\",\"targets\":[{\"name\":\"app\",\"kind\":\"executable\","
             "\"sources\":[{\"path\":\"src/main.c\",\"language\":\"c\"}]}]}]}\n",
             origin);
}

/* The scripts above carry a %ROOT% placeholder, because a document's root has to
   be the directory the test actually made and a shell script cannot know it
   until then. */
static bool install_answering(const sandbox *box, const char *name, const char *extension,
                              const char *origin) {
    char script[2048];
    answering_script(script, sizeof script, origin);

    char *at = strstr(script, "%ROOT%");
    if(at == NULL)
        return false;
    char patched[4096];
    snprintf(patched, sizeof patched, "%.*s%s%s", (int)(at - script), script, box->project,
             at + strlen("%ROOT%"));

    char spec[1024];
    if(!frontend_spec(box, patched, 0, 0, NULL, spec, sizeof spec))
        return false;

    char recipe[1024];
    frontend_recipe(recipe, sizeof recipe, name, extension, IR_SCHEMA, "0.1.0");
    return install(box, name, spec, recipe);
}

/* Put the file that makes a plugin a candidate into the project directory. */
static bool touch_entry(const sandbox *box, const char *filename) {
    char path[320];
    snprintf(path, sizeof path, "%s/%s", box->project, filename);
    return fs_write_file(path, "# a build file molto does not read\n");
}

/* --- capabilities and selection --- */

MOLTEST(frontend_declares_answers_from_the_recipe) {
    recipe_plugin plugin;
    memset(&plugin, 0, sizeof plugin);
    snprintf(plugin.capabilities[0], RECIPE_PLUGIN_ENTRY_MAX, "frontend");
    snprintf(plugin.capabilities[1], RECIPE_PLUGIN_ENTRY_MAX, "generator");
    plugin.capability_count = 2;

    EXPECT_TRUE(frontend_declares(&plugin, "frontend"));
    EXPECT_TRUE(frontend_declares(&plugin, "generator"));
    EXPECT_FALSE(frontend_declares(&plugin, "packager"));
    EXPECT_FALSE(frontend_declares(&plugin, NULL));
    EXPECT_FALSE(frontend_declares(NULL, "frontend"));
}

MOLTEST(frontend_candidates_needs_the_file_to_be_there) {
    /* An extension is a filename, and a plugin whose filename is not in the
       directory is not a candidate for it. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    ASSERT_TRUE(install_answering(&box, "meson", "meson.build", "meson"));

    frontend_choice found[FRONTEND_MAX_CANDIDATES];
    size_t count = 99;
    EXPECT_TRUE(frontend_candidates(box.project, found, FRONTEND_MAX_CANDIDATES, &count));
    EXPECT_EQ(0u, count);

    ASSERT_TRUE(touch_entry(&box, "meson.build"));
    EXPECT_TRUE(frontend_candidates(box.project, found, FRONTEND_MAX_CANDIDATES, &count));
    ASSERT_EQ(1u, count);
    EXPECT_STREQ("meson", found[0].name);
    EXPECT_STREQ("meson.build", found[0].entry);

    sandbox_teardown(&box);
}

MOLTEST(frontend_candidates_skips_a_plugin_that_is_not_a_frontend) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char recipe[1024];
    frontend_recipe(recipe, sizeof recipe, "deb", "meson.build", IR_SCHEMA, "0.1.0");
    /* A packager, not a frontend. Being installed is not being asked. */
    char *at = strstr(recipe, "\"frontend\"");
    ASSERT_NOT_NULL(at);
    memcpy(at, "\"packager\"", strlen("\"packager\""));
    ASSERT_TRUE(install(&box, "deb", "exit 0\n", recipe));
    ASSERT_TRUE(touch_entry(&box, "meson.build"));

    frontend_choice found[FRONTEND_MAX_CANDIDATES];
    size_t count = 99;
    EXPECT_TRUE(frontend_candidates(box.project, found, FRONTEND_MAX_CANDIDATES, &count));
    EXPECT_EQ(0u, count);

    sandbox_teardown(&box);
}

MOLTEST(frontend_refuses_a_schema_it_cannot_exchange_with) {
    /* Refused before the process starts. A mismatch found here is a refusal;
       found halfway through a document it is a half-read document. */
    frontend_choice choice;
    memset(&choice, 0, sizeof choice);
    snprintf(choice.name, sizeof choice.name, "meson");
    choice.plugin.ir_schema = IR_SCHEMA + 1;

    char err[512] = "";
    EXPECT_FALSE(frontend_compatible(&choice, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "schema"));

    choice.plugin.ir_schema = IR_SCHEMA;
    EXPECT_TRUE(frontend_compatible(&choice, err, sizeof err));
}

MOLTEST(frontend_refuses_a_plugin_that_needs_a_newer_molto) {
    frontend_choice choice;
    memset(&choice, 0, sizeof choice);
    snprintf(choice.name, sizeof choice.name, "meson");
    choice.plugin.ir_schema = IR_SCHEMA;
    snprintf(choice.plugin.molto_min, sizeof choice.plugin.molto_min, "99.0.0");

    char err[512] = "";
    EXPECT_FALSE(frontend_compatible(&choice, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "99.0.0"));
}

/* --- asking one --- */

/* Bounds for the sandbox project, which is what a caller supplies. Zeroed
   first: every field not named here has to be absent rather than whatever the
   stack held, and a caller that sets them one by one has no other way to say
   so. */
static void bounds_for(const sandbox *box, char *build_dir, size_t size, ir_bounds *out) {
    snprintf(build_dir, size, "%s/build/debug", box->project);
    *out = (ir_bounds){.workspace = box->project, .build_dir = build_dir};
}

MOLTEST(frontend_reads_the_document_a_plugin_returns) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(install_answering(&box, "meson", "meson.build", "meson"));
    ASSERT_TRUE(touch_entry(&box, "meson.build"));

    frontend_choice found[FRONTEND_MAX_CANDIDATES];
    size_t count = 0;
    ASSERT_TRUE(frontend_candidates(box.project, found, FRONTEND_MAX_CANDIDATES, &count));
    ASSERT_EQ(1u, count);

    char build_dir[320];
    ir_bounds bounds;
    bounds_for(&box, build_dir, sizeof build_dir, &bounds);

    ir_document doc;
    char err[1024] = "";
    ASSERT_EQ(frontend_ok, frontend_ask(&found[0], box.project, &bounds, &doc, err, sizeof err));
    EXPECT_STREQ("", err);

    EXPECT_STREQ("app", doc.name);
    EXPECT_STREQ("meson", doc.origin);
    EXPECT_TRUE(ir_is_from_plugin(&doc));
    ASSERT_EQ(1u, doc.target_count);
    ASSERT_EQ(1u, doc.targets[0].source_count);
    EXPECT_STREQ("src/main.c", doc.targets[0].sources[0].path);
    ASSERT_EQ(1u, str_list_count(&doc.files_read));
    EXPECT_STREQ("meson.build", str_list_get(&doc.files_read, 0));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

/* Install a plugin whose script is given outright, and ask it. */
/* Three shapes used inline, each returning a spec that lives long enough for
   the call it is handed to. */
static const char *spec_exiting(const sandbox *box, int code) {
    static char spec[1024];
    return frontend_spec(box, NULL, code, 0, NULL, spec, sizeof spec) ? spec : "exit 1\n";
}

static const char *spec_printing(const sandbox *box, const char *text) {
    static char spec[1024];
    return frontend_spec(box, text, 0, 0, NULL, spec, sizeof spec) ? spec : "exit 1\n";
}

/* Longer than the one-second timeout ask_script allows, so the caller has to be
   what stops it. */
static const char *spec_hanging(const sandbox *box) {
    static char spec[1024];
    return frontend_spec(box, NULL, 0, 60000u, NULL, spec, sizeof spec) ? spec : "exit 1\n";
}

static frontend_result ask_script(sandbox *box, const char *script, ir_document *out, char *err,
                                  size_t err_size) {
    /* Before anything that can fail. Every caller frees this afterwards, and
       three of the returns below are reached without `frontend_ask_with` ever
       being called — so on those paths the caller was freeing whatever the
       stack happened to hold. On Linux that was zeroes and the free was a
       no-op; on Windows it was a pointer, and a failing assertion became a
       crash that took the rest of the suite with it. */
    ir_document_init(out);

    char recipe[1024];
    frontend_recipe(recipe, sizeof recipe, "meson", "meson.build", IR_SCHEMA, "0.1.0");
    if(!install(box, "meson", script, recipe) || !touch_entry(box, "meson.build"))
        return frontend_failed;

    frontend_choice found[FRONTEND_MAX_CANDIDATES];
    size_t count = 0;
    if(!frontend_candidates(box->project, found, FRONTEND_MAX_CANDIDATES, &count) || count != 1)
        return frontend_failed;

    char build_dir[320];
    ir_bounds bounds;
    bounds_for(box, build_dir, sizeof build_dir, &bounds);
    /* A second is thirty times less than a build allows and thirty times more
       than any script here needs, so the timeout test costs a second rather
       than half a minute. */
    return frontend_ask_with(&found[0], box->project, &bounds, 1000u, out, err, err_size);
}

MOLTEST(frontend_treats_exit_three_as_declining) {
    /* Not an error: the file is not one it understands, and Molto is free to
       try another candidate. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_none,
              ask_script(&box, spec_exiting(&box, 3), &doc, err, sizeof err));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_fails_on_any_other_exit) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_failed,
              ask_script(&box, spec_exiting(&box, 1), &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "exited 1"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_refuses_a_banner_on_standard_output) {
    /* Standard output MUST be a document and nothing else. A plugin that greets
       there has produced an unparseable document, and anything it wants to say
       goes to standard error. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_failed,
              ask_script(&box,
                         spec_printing(&box, "molto-meson 0.1.0\n{\"schema\":3}\n"),
                         &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "JSON"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_refuses_a_plugin_that_claims_to_be_native) {
    /* The load-bearing check. Every stricter lowering rule keys on the origin
       not being "native", so a plugin naming itself native would be handed the
       rules written for a file in the user's own repository. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(install_answering(&box, "meson", "meson.build", IR_ORIGIN_NATIVE));
    ASSERT_TRUE(touch_entry(&box, "meson.build"));

    frontend_choice found[FRONTEND_MAX_CANDIDATES];
    size_t count = 0;
    ASSERT_TRUE(frontend_candidates(box.project, found, FRONTEND_MAX_CANDIDATES, &count));
    ASSERT_EQ(1u, count);

    char build_dir[320];
    ir_bounds bounds;
    bounds_for(&box, build_dir, sizeof build_dir, &bounds);

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_failed,
              frontend_ask(&found[0], box.project, &bounds, &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "origin"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_refuses_a_document_reporting_no_files_read) {
    /* RFC-0013 makes it the invalidation key of a cached document, and a
       frontend written against a molto that did not ask would be one that never
       learned to answer. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char script[2048];
    snprintf(script, sizeof script,
             "{\"schema\":3,\"files_read\":[],\"projects\":[{\"name\":\"app\","
             "\"version\":\"0.1.0\",\"root\":\"%s\",\"origin\":\"meson\"}]}\n",
                      box.project);

    char spec[2048];
    ASSERT_TRUE(frontend_spec(&box, script, 0, 0, NULL, spec, sizeof spec));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_failed, ask_script(&box, spec, &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "files read"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_validates_before_it_hands_a_document_back) {
    /* The check is not the caller's to remember. A frontend that returns a
       source outside the workspace has its document refused here, not later. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char script[2048];
    snprintf(script, sizeof script,
             "{\"schema\":3,\"files_read\":[\"meson.build\"],\"projects\":[{\"name\":\"app\","
             "\"version\":\"0.1.0\",\"root\":\"%s\",\"origin\":\"meson\","
             "\"targets\":[{\"name\":\"app\",\"kind\":\"executable\","
             "\"sources\":[{\"path\":\"../../etc/shadow\",\"language\":\"c\"}]}]}]}\n",
                      box.project);

    char spec[2048];
    ASSERT_TRUE(frontend_spec(&box, script, 0, 0, NULL, spec, sizeof spec));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_failed, ask_script(&box, spec, &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "outside the workspace"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_refuses_a_plugin_that_names_a_dependency) {
    /* A frontend describes a project and not its graph. `Dependency` carries
       the version that was resolved, the origin it came from and where the
       bytes landed on this machine — all three are answers `resolve` gives, and
       `resolve` is the phase RFC-0015 closes to plugins. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char script[2048];
    snprintf(script, sizeof script,
             "{\"schema\":3,\"files_read\":[\"meson.build\"],\"projects\":[{\"name\":\"app\","
             "\"version\":\"0.1.0\",\"root\":\"%s\",\"origin\":\"meson\",\"targets\":[],"
             "\"dependencies\":[{\"name\":\"z\",\"version\":\"1.0.0\",\"origin\":\"registry\","
             "\"scope\":\"runtime\",\"root\":\"%s\"}]}]}\n",
             box.project, box.project);

    char spec[2048];
    ASSERT_TRUE(frontend_spec(&box, script, 0, 0, NULL, spec, sizeof spec));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_failed, ask_script(&box, spec, &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "not its graph"));
    /* Singular, because one dependency is one dependency. */
    EXPECT_NOT_NULL(strstr(err, "naming 1 dependency"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_refuses_a_plugin_option_that_loads_code_into_the_compiler) {
    /* The sandbox that would stop this does not exist yet, and it would not
       stop this anyway: the option is in a document, not in a syscall. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char script[2048];
    snprintf(script, sizeof script,
             "{\"schema\":3,\"files_read\":[\"meson.build\"],\"projects\":[{\"name\":\"app\","
             "\"version\":\"0.1.0\",\"root\":\"%s\",\"origin\":\"meson\","
             "\"targets\":[{\"name\":\"app\",\"kind\":\"executable\","
             "\"options\":[{\"value\":\"-fplugin=/tmp/x.so\",\"scope\":\"target\"}]}]}]}\n",
                      box.project);

    char spec[2048];
    ASSERT_TRUE(frontend_spec(&box, script, 0, 0, NULL, spec, sizeof spec));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_failed, ask_script(&box, spec, &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "a plugin may not"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_refuses_a_build_step_from_a_plugin_that_only_reads) {
    /* A `custom_target` becomes a BuildStep, and a BuildStep needs `generator`.
       This revision carries no BuildStep at all, so it is refused at the node
       — never by falling back to the real tool. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char script[2048];
    snprintf(script, sizeof script,
             "{\"schema\":3,\"files_read\":[\"meson.build\"],\"projects\":[{\"name\":\"app\","
             "\"version\":\"0.1.0\",\"root\":\"%s\",\"origin\":\"meson\","
             "\"steps\":[{\"name\":\"gen\",\"program\":\"sh\",\"args\":[\"-c\",\"rm -rf ~\"]}]}]}\n",
                      box.project);

    char spec[2048];
    ASSERT_TRUE(frontend_spec(&box, script, 0, 0, NULL, spec, sizeof spec));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_failed, ask_script(&box, spec, &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "BuildStep"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_stops_a_plugin_that_never_answers) {
    /* A plugin that hangs must not hang a build. The wait here is the real
       timeout, so this test costs what the timeout costs and no more. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_failed,
              ask_script(&box, spec_hanging(&box), &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "was stopped"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

/* --- the request a frontend receives --- */

MOLTEST(frontend_tells_the_plugin_which_directory_and_which_file) {
    /* The request is the first thing any plugin author meets, so its shape is
       pinned here rather than left to whatever the writer happened to emit. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char log[320];
    snprintf(log, sizeof log, "%s/request", box.root);

    static char script[1024];
    ASSERT_TRUE(frontend_spec(&box, NULL, 3, 0, log, script, sizeof script));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_none, ask_script(&box, script, &doc, err, sizeof err));
    ir_document_free(&doc);

    char *seen = fs_read_file(log);
    ASSERT_NOT_NULL(seen);
    /* The capability is the subcommand, so a plugin providing several does not
       have to infer which one from the document. */
    EXPECT_NOT_NULL(strstr(seen, "argv1=frontend"));
    EXPECT_NOT_NULL(strstr(seen, "\"request\": \"frontend\""));
    EXPECT_NOT_NULL(strstr(seen, "\"entry\": \"meson.build\""));
    EXPECT_NOT_NULL(strstr(seen, box.project));
    free(seen);

    sandbox_teardown(&box);
}

/* --- the native frontend --- */

MOLTEST(frontend_native_describes_a_manifest_and_its_sources) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char manifest[320];
    snprintf(manifest, sizeof manifest, "%s/Project.toml", box.project);
    ASSERT_TRUE(fs_write_file(manifest, "[package]\n"
                                        "name = \"app\"\n"
                                        "version = \"1.2.3\"\n"
                                        "[target]\n"
                                        "std = \"c17\"\n"
                                        "include = [\"include\"]\n"
                                        "defines = [\"APP=1\"]\n"
                                        "link = [\"m\"]\n"));

    char src[320];
    snprintf(src, sizeof src, "%s/src", box.project);
    ASSERT_TRUE(fs_make_dirs(src));
    char main_c[384];
    snprintf(main_c, sizeof main_c, "%s/main.c", src);
    ASSERT_TRUE(fs_write_file(main_c, "int main(void){return 0;}\n"));
    char extra[384];
    snprintf(extra, sizeof extra, "%s/extra.cpp", src);
    ASSERT_TRUE(fs_write_file(extra, "int extra(){return 0;}\n"));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "debug", &doc, err, sizeof err));
    EXPECT_STREQ("", err);

    EXPECT_STREQ("app", doc.name);
    EXPECT_STREQ("1.2.3", doc.version);
    EXPECT_STREQ(IR_ORIGIN_NATIVE, doc.origin);
    EXPECT_FALSE(ir_is_from_plugin(&doc));
    ASSERT_EQ(1u, str_list_count(&doc.files_read));
    EXPECT_STREQ("Project.toml", str_list_get(&doc.files_read, 0));

    ASSERT_EQ(1u, doc.target_count);
    const ir_target *target = &doc.targets[0];
    EXPECT_STREQ("app", target->name);
    EXPECT_EQ(ir_target_executable, target->kind);

    /* Sorted, so two runs produce one byte-identical document however the
       filesystem felt about the order. */
    ASSERT_EQ(2u, target->source_count);
    EXPECT_STREQ("src/extra.cpp", target->sources[0].path);
    EXPECT_EQ(ir_language_cpp, target->sources[0].language);
    EXPECT_STREQ("src/main.c", target->sources[1].path);
    EXPECT_EQ(ir_language_c, target->sources[1].language);

    bool saw_define = false;
    for(size_t i = 0; i < target->option_count; i++) {
        saw_define = saw_define || strcmp(target->options[i].value, "-DAPP=1") == 0;
        /* The standard is not target scope: a target holds units of both
           languages and there are two standards to state. */
        EXPECT_FALSE(strncmp(target->options[i].value, "-std=", 5) == 0);
    }
    EXPECT_TRUE(saw_define);

    /* `std` reaches the C unit, and the C++ one gets nothing because this
       manifest declares no `cpp_std`. Stating `-std=c17` for it would not be a
       detail: it would be the wrong language. */
    ASSERT_EQ(0u, target->sources[0].option_count); /* src/extra.cpp */
    ASSERT_EQ(1u, target->sources[1].option_count); /* src/main.c */
    EXPECT_STREQ("-std=c17", target->sources[1].options[0].value);
    EXPECT_EQ(ir_scope_unit, target->sources[1].options[0].scope);

    /* What the manifest named, and then `src/`, which every build has on the
       include path and no manifest states. */
    ASSERT_EQ(2u, target->include_count);
    EXPECT_STREQ("include", target->includes[0].value);
    EXPECT_STREQ("src", target->includes[1].value);
    /* `[target].link = ["m"]`, as it reaches the link line. */
    ASSERT_EQ(1u, target->link_count);
    EXPECT_STREQ("-lm", target->links[0].value);
    EXPECT_TRUE(target->has_artifact);
    EXPECT_STREQ("app", target->artifact.path);

    /* Its dependencies are a transform's job, and this revision has none. */
    EXPECT_EQ(0u, doc.dependency_count);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_native_leaves_out_what_the_profile_decides) {
    /* -O and -g come from a profile's opt_level and debug_info, which are the
       build's mechanics rather than anything the project said. Stating them
       here would state one thing in two places, and eventually in two ways. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char manifest[320];
    snprintf(manifest, sizeof manifest, "%s/Project.toml", box.project);
    ASSERT_TRUE(fs_write_file(manifest, "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
                                        "[profile.release]\nopt_level = 3\ndebug_info = false\n"));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "release", &doc, err, sizeof err));

    ASSERT_EQ(1u, doc.target_count);
    for(size_t i = 0; i < doc.targets[0].option_count; i++) {
        const char *value = doc.targets[0].options[i].value;
        if(strncmp(value, "-O", 2) == 0 || strcmp(value, "-g") == 0)
            FAIL("the document carries a profile flag the engine composes");
    }

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_native_states_each_language_its_own_standard) {
    /* `[target]` declares two standards and a target holds units of both
       languages, which is why the standard is a unit-scope option: one at
       target scope would state one of them for all of them. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char manifest[320];
    snprintf(manifest, sizeof manifest, "%s/Project.toml", box.project);
    ASSERT_TRUE(fs_write_file(manifest, "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
                                        "[target]\nstd = \"c17\"\ncpp_std = \"c++20\"\n"));

    char path[384];
    snprintf(path, sizeof path, "%s/src", box.project);
    ASSERT_TRUE(fs_make_dirs(path));
    snprintf(path, sizeof path, "%s/src/a.c", box.project);
    ASSERT_TRUE(fs_write_file(path, "int a;\n"));
    snprintf(path, sizeof path, "%s/src/b.cpp", box.project);
    ASSERT_TRUE(fs_write_file(path, "int b;\n"));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "debug", &doc, err, sizeof err));

    ASSERT_EQ(1u, doc.target_count);
    const ir_target *target = &doc.targets[0];
    ASSERT_EQ(2u, target->source_count);

    EXPECT_STREQ("src/a.c", target->sources[0].path);
    ASSERT_EQ(1u, target->sources[0].option_count);
    EXPECT_STREQ("-std=c17", target->sources[0].options[0].value);

    EXPECT_STREQ("src/b.cpp", target->sources[1].path);
    ASSERT_EQ(1u, target->sources[1].option_count);
    EXPECT_STREQ("-std=c++20", target->sources[1].options[0].value);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

/* --- the tests, as targets --- */

/* A project with a manifest, a source and whatever test files are named. */
static bool project_with_tests(const sandbox *box, const char *manifest, const char *const *tests,
                               size_t count) {
    char path[384];
    snprintf(path, sizeof path, "%s/Project.toml", box->project);
    if(!fs_write_file(path, manifest))
        return false;

    snprintf(path, sizeof path, "%s/src", box->project);
    if(!fs_make_dirs(path))
        return false;
    snprintf(path, sizeof path, "%s/src/main.c", box->project);
    if(!fs_write_file(path, "int main(void){return 0;}\n"))
        return false;

    for(size_t i = 0; i < count; i++) {
        char file[512];
        snprintf(file, sizeof file, "%s/%s", box->project, tests[i]);
        char *slash = strrchr(file, '/');
        if(slash != NULL) {
            *slash = '\0';
            if(!fs_make_dirs(file))
                return false;
            *slash = '/';
        }
        if(!fs_write_file(file, "int main(void){return 0;}\n"))
            return false;
    }
    return true;
}

/* The target named `name`, or NULL. A test that indexed by position would break
   the moment a target is added, and would break silently. */
static const ir_target *target_named(const ir_document *doc, const char *name) {
    for(size_t i = 0; i < doc->target_count; i++) {
        if(doc->targets[i].name != NULL && strcmp(doc->targets[i].name, name) == 0)
            return &doc->targets[i];
    }
    return NULL;
}

static bool carries_option(const ir_target *target, const char *value) {
    for(size_t i = 0; i < target->option_count; i++) {
        if(strcmp(target->options[i].value, value) == 0)
            return true;
    }
    return false;
}

MOLTEST(frontend_native_describes_one_target_per_test_file) {
    /* The default mode: each file brings its own main(), so each is a binary,
       so each is a target. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    const char *tests[] = {"tests/test_json.c", "tests/deep/test_io.cpp"};
    ASSERT_TRUE(project_with_tests(
        &box, "[package]\nname = \"app\"\nversion = \"1.0.0\"\n[target]\nstd = \"c17\"\n", tests,
        2));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "debug", &doc, err, sizeof err));
    EXPECT_STREQ("", err);

    ASSERT_EQ(3u, doc.target_count);

    /* Named by the whole stem, which is where `molto test` puts the binary and
       is what keeps two files of one basename from being one name. */
    const ir_target *json = target_named(&doc, "tests/test_json");
    ASSERT_TRUE(json != NULL);
    EXPECT_EQ(ir_target_test, json->kind);
    ASSERT_EQ(1u, json->source_count);
    EXPECT_STREQ("tests/test_json.c", json->sources[0].path);
    EXPECT_EQ(ir_language_c, json->sources[0].language);
    EXPECT_TRUE(json->has_artifact);
    EXPECT_EQ(ir_target_test, json->artifact.kind);
    EXPECT_STREQ("tests/test_json", json->artifact.path);

    /* The edge that says where the rest of its objects come from. */
    ASSERT_EQ(1u, str_list_count(&json->depends_on));
    EXPECT_STREQ("app", str_list_get(&json->depends_on, 0));

    /* A nested C++ test keeps both its directory and its language. */
    const ir_target *io = target_named(&doc, "tests/deep/test_io");
    ASSERT_TRUE(io != NULL);
    ASSERT_EQ(1u, io->source_count);
    EXPECT_STREQ("tests/deep/test_io.cpp", io->sources[0].path);
    EXPECT_EQ(ir_language_cpp, io->sources[0].language);
    EXPECT_STREQ("tests/deep/test_io", io->artifact.path);

    /* The executable is untouched by any of it. */
    const ir_target *app = target_named(&doc, "app");
    ASSERT_TRUE(app != NULL);
    EXPECT_EQ(ir_target_executable, app->kind);
    ASSERT_EQ(1u, app->source_count);
    EXPECT_STREQ("src/main.c", app->sources[0].path);
    EXPECT_EQ(0u, str_list_count(&app->depends_on));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_native_describes_a_single_suite_when_asked_to) {
    /* mode = "single" is one binary for every test file, which is what a
       framework owning main() needs — so it is one target. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    const char *tests[] = {"tests/test_a.c", "tests/test_b.c"};
    ASSERT_TRUE(project_with_tests(&box,
                                   "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
                                   "[test]\nmode = \"single\"\n",
                                   tests, 2));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "debug", &doc, err, sizeof err));

    ASSERT_EQ(2u, doc.target_count);
    const ir_target *suite = target_named(&doc, "app_tests");
    ASSERT_TRUE(suite != NULL);
    EXPECT_EQ(ir_target_test, suite->kind);
    EXPECT_STREQ("tests/app_tests", suite->artifact.path);

    ASSERT_EQ(2u, suite->source_count);
    EXPECT_STREQ("tests/test_a.c", suite->sources[0].path);
    EXPECT_STREQ("tests/test_b.c", suite->sources[1].path);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_native_describes_a_framework_outside_the_tests_directory) {
    /* `[test].sources` is how a framework living outside tests/ gets compiled
       in, and a document that left it out would describe a suite that does not
       link. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    const char *tests[] = {"tests/test_a.c", "vendor/moltest/moltest.c"};
    ASSERT_TRUE(project_with_tests(&box,
                                   "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
                                   "[test]\nmode = \"single\"\n"
                                   "sources = [\"vendor/moltest\"]\n",
                                   tests, 2));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "debug", &doc, err, sizeof err));

    const ir_target *suite = target_named(&doc, "app_tests");
    ASSERT_TRUE(suite != NULL);
    ASSERT_EQ(2u, suite->source_count);
    EXPECT_STREQ("tests/test_a.c", suite->sources[0].path);
    EXPECT_STREQ("vendor/moltest/moltest.c", suite->sources[1].path);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_native_keeps_test_options_out_of_the_executable) {
    /* `[test]`'s own defines, includes and flags reach the tests and nothing
       else. That separation is the whole point of the table, and a document
       that folded them into the executable would describe a binary shipping
       code that only its tests were meant to see. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    const char *tests[] = {"tests/test_a.c"};
    ASSERT_TRUE(project_with_tests(&box,
                                   "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
                                   "[target]\ndefines = [\"APP=1\"]\n"
                                   "[test]\ndefines = [\"TESTING=1\"]\n"
                                   "include = [\"tests/support\"]\n",
                                   tests, 1));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "debug", &doc, err, sizeof err));

    const ir_target *test = target_named(&doc, "tests/test_a");
    ASSERT_TRUE(test != NULL);
    EXPECT_TRUE(carries_option(test, "-DTESTING=1"));
    /* And what the whole project said, because a test compiles the project. */
    EXPECT_TRUE(carries_option(test, "-DAPP=1"));

    bool saw_support = false;
    for(size_t i = 0; i < test->include_count; i++)
        saw_support = saw_support || strcmp(test->includes[i].value, "tests/support") == 0;
    EXPECT_TRUE(saw_support);

    const ir_target *app = target_named(&doc, "app");
    ASSERT_TRUE(app != NULL);
    EXPECT_FALSE(carries_option(app, "-DTESTING=1"));
    EXPECT_TRUE(carries_option(app, "-DAPP=1"));
    for(size_t i = 0; i < app->include_count; i++)
        EXPECT_FALSE(strcmp(app->includes[i].value, "tests/support") == 0);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_native_describes_no_test_target_when_there_are_no_tests) {
    /* Not an empty one: a target that builds nothing is a target every consumer
       has to special-case, and "there are no tests" is said by there being
       none. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    ASSERT_TRUE(project_with_tests(&box, "[package]\nname = \"app\"\nversion = \"1.0.0\"\n",
                                   NULL, 0));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "debug", &doc, err, sizeof err));

    ASSERT_EQ(1u, doc.target_count);
    EXPECT_STREQ("app", doc.targets[0].name);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_native_refuses_a_test_source_that_is_not_there) {
    /* A missing tests/ is nothing; a `[test].sources` entry naming a file that
       does not exist is a manifest describing a build that cannot happen. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    ASSERT_TRUE(project_with_tests(&box,
                                   "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
                                   "[test]\nsources = [\"vendor/gone.c\"]\n",
                                   NULL, 0));

    ir_document doc;
    char err[1024] = "";
    EXPECT_FALSE(frontend_native(box.project, "debug", &doc, err, sizeof err));
    EXPECT_TRUE(strstr(err, "vendor/gone.c") != NULL);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_native_produces_a_document_that_validates) {
    /* Every rule ir_validate applies to a document applies to this one: unique
       names, an edge that resolves, no cycle, every path inside the bounds. A
       frontend whose own answer would be refused from a plugin is a frontend
       writing a document nobody can execute. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    const char *tests[] = {"tests/test_a.c", "tests/test_b.c"};
    ASSERT_TRUE(project_with_tests(&box, "[package]\nname = \"app\"\nversion = \"1.0.0\"\n",
                                   tests, 2));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "debug", &doc, err, sizeof err));

    char build_dir[320];
    ir_bounds bounds;
    bounds_for(&box, build_dir, sizeof build_dir, &bounds);
    EXPECT_TRUE(ir_validate(&doc, &bounds, err, sizeof err));
    EXPECT_STREQ("", err);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_native_puts_src_on_the_include_path_last) {
    /* Every Molto build has `src/` on the include path and no manifest says
       so, which is what lets a source include a sibling by name. It is stated
       last because that is where the build puts it, and include order decides
       which header wins when two directories carry the same name. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    const char *tests[] = {"tests/test_a.c"};
    ASSERT_TRUE(project_with_tests(&box,
                                   "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
                                   "[target]\ninclude = [\"include\"]\n"
                                   "[test]\ninclude = [\"tests/support\"]\n",
                                   tests, 1));

    ir_document doc;
    char err[1024] = "";
    ASSERT_TRUE(frontend_native(box.project, "debug", &doc, err, sizeof err));

    const ir_target *app = target_named(&doc, "app");
    ASSERT_TRUE(app != NULL);
    ASSERT_EQ(2u, app->include_count);
    EXPECT_STREQ("include", app->includes[0].value);
    EXPECT_STREQ("src", app->includes[1].value);
    EXPECT_FALSE(app->includes[1].system); /* the project's own headers: -I, never -isystem */

    /* And after `[test]`'s own, which is the order the compile line carries. */
    const ir_target *test = target_named(&doc, "tests/test_a");
    ASSERT_TRUE(test != NULL);
    ASSERT_EQ(3u, test->include_count);
    EXPECT_STREQ("include", test->includes[0].value);
    EXPECT_STREQ("tests/support", test->includes[1].value);
    EXPECT_STREQ("src", test->includes[2].value);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

/* --- precedence --- */

MOLTEST(frontend_run_prefers_the_native_manifest) {
    /* A plugin cannot take over a directory molto already understands, the same
       rule the CLI applies to a command name and for the same reason. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(install_answering(&box, "meson", "meson.build", "meson"));
    ASSERT_TRUE(touch_entry(&box, "meson.build"));

    char manifest[320];
    snprintf(manifest, sizeof manifest, "%s/Project.toml", box.project);
    ASSERT_TRUE(fs_write_file(manifest, "[package]\nname = \"mine\"\nversion = \"1.0.0\"\n"));

    ir_document doc;
    char err[1024] = "";
    ASSERT_EQ(frontend_ok, frontend_run(box.project, "debug", &doc, err, sizeof err));
    EXPECT_STREQ("mine", doc.name);
    EXPECT_STREQ(IR_ORIGIN_NATIVE, doc.origin);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_run_asks_a_plugin_when_there_is_no_manifest) {
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(install_answering(&box, "meson", "meson.build", "meson"));
    ASSERT_TRUE(touch_entry(&box, "meson.build"));

    ir_document doc;
    char err[1024] = "";
    ASSERT_EQ(frontend_ok, frontend_run(box.project, "debug", &doc, err, sizeof err));
    EXPECT_STREQ("app", doc.name);
    EXPECT_STREQ("meson", doc.origin);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_run_says_so_when_nothing_understands_the_directory) {
    /* Not an error: a directory no frontend understands is one molto has
       nothing to say about. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_none, frontend_run(box.project, "debug", &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "no Project.toml"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_run_reports_a_bad_manifest_as_a_manifest_problem) {
    /* Nothing third-party ran, so this is not a plugin failure. Telling the two
       apart is what the enumerated exit codes exist for. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));

    char manifest[320];
    snprintf(manifest, sizeof manifest, "%s/Project.toml", box.project);
    /* A typo in a key, which `[package]` fails closed on (RFC-0003) — the one
       table in the manifest that does, and for exactly this reason. */
    ASSERT_TRUE(fs_write_file(manifest, "[package]\nname = \"app\"\n"
                                        "version = \"1.0.0\"\nnmae = \"typo\"\n"));

    ir_document doc;
    char err[1024] = "";
    EXPECT_EQ(frontend_bad_manifest, frontend_run(box.project, "debug", &doc, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "nmae"));

    ir_document_free(&doc);
    sandbox_teardown(&box);
}

MOLTEST(frontend_run_accepts_a_relative_directory) {
    /* A regression, and one no test with an absolute sandbox path could have
       caught: `molto ir` in a directory with no manifest had nothing to walk up
       to and passed the working directory through relative. The bounds a
       document is validated against were then relative while the document's own
       paths were absolute, and every correct document was rejected as escaping
       a workspace it was inside. */
    sandbox box;
    ASSERT_TRUE(sandbox_setup(&box));
    ASSERT_TRUE(install_answering(&box, "meson", "meson.build", "meson"));
    ASSERT_TRUE(touch_entry(&box, "meson.build"));

    char previous[4096];
    ASSERT_NOT_NULL(getcwd(previous, sizeof previous));
    ASSERT_EQ(0, chdir(box.project));

    ir_document doc;
    char err[1024] = "";
    const frontend_result result = frontend_run(".", "debug", &doc, err, sizeof err);

    ASSERT_EQ(0, chdir(previous));

    EXPECT_EQ(frontend_ok, result);
    EXPECT_STREQ("", err);
    EXPECT_STREQ("app", doc.name);

    ir_document_free(&doc);
    sandbox_teardown(&box);
}
