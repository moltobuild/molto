#include <molto/commands/plugin_command.h>

#include <molto/cli.h>
#include <molto/exit_code.h>
#include <molto/services/plugin_service.h>
#include <molto/util/progress.h>

#include <stdio.h>
#include <string.h>

/* --- rendering --- */

static const char *origin_word(plugin_origin origin) {
    return origin == plugin_origin_installed ? "installed" : "on PATH";
}

/* Print a list the way the recipe wrote it, or a stand-in when there is none.
   The two are deliberately different words: an empty list is a plugin that
   declared it needs nothing, and a missing recipe is nobody knowing. */
static void print_list(const char *label, const char list[][RECIPE_PLUGIN_ENTRY_MAX],
                       size_t count) {
    printf("  %-13s", label);
    if(count == 0) {
        printf("none declared\n");
        return;
    }
    for(size_t i = 0; i < count; i++)
        printf("%s%s", i > 0 ? ", " : "", list[i]);
    printf("\n");
}

/* The one line `list` gives a plugin: name, origin, and what it can do. */
static void print_summary(const plugin_entry *entry) {
    printf("  %-16s %-10s ", entry->name, origin_word(entry->origin));

    /* A plugin whose name is one of Molto's own is unreachable: the command
       table is searched first, always. Saying so is the whole reason to report
       it at all — a listing that showed it like any other would be naming a
       command that cannot be run. */
    if(cli_has_command(entry->name)) {
        printf("(shadowed by the built-in '%s': it will never run)\n", entry->name);
        return;
    }

    recipe_plugin plugin;
    char err[256] = "";
    if(!entry->has_recipe || !plugin_read_recipe(entry->name, NULL, &plugin, err, sizeof err)) {
        printf("(no recipe: capabilities and permissions unknown)\n");
        return;
    }
    for(size_t i = 0; i < plugin.capability_count; i++)
        printf("%s%s", i > 0 ? "," : "", plugin.capabilities[i]);
    printf("\n");
}

/* --- list --- */

static int run_list(void) {
    plugin_entry entries[PLUGIN_MAX_LISTED];
    size_t count = 0;
    if(!plugin_list(entries, PLUGIN_MAX_LISTED, &count)) {
        fprintf(stderr,
                "molto: more than %d plugins are installed, which is more than this "
                "can list\n",
                PLUGIN_MAX_LISTED);
        return exit_plugin_failure;
    }

    if(count == 0) {
        printf("No plugins installed.\n");
        return exit_ok;
    }

    for(size_t i = 0; i < count; i++)
        print_summary(&entries[i]);
    return exit_ok;
}

/* --- info --- */

/* Where a plugin is, and how it got there, without reading any recipe. */
static bool locate(const char *name, plugin_entry *out) {
    plugin_entry entries[PLUGIN_MAX_LISTED];
    size_t count = 0;
    if(!plugin_list(entries, PLUGIN_MAX_LISTED, &count))
        return false;

    for(size_t i = 0; i < count; i++) {
        if(strcmp(entries[i].name, name) == 0) {
            *out = entries[i];
            return true;
        }
    }
    return false;
}

static int run_info(const char *name) {
    if(name == NULL) {
        fprintf(stderr, "molto: plugin info needs a name\n");
        return exit_usage_error;
    }

    plugin_entry entry;
    if(!locate(name, &entry)) {
        fprintf(stderr, "molto: no plugin named '%s'\n", name);
        return exit_dependency_failure;
    }

    printf("%s\n", entry.name);
    printf("  %-13s%s\n", "path", entry.path);
    printf("  %-13s%s\n", "origin", origin_word(entry.origin));
    if(cli_has_command(name))
        printf("  %-13s%s\n", "reachable", "no: shadowed by the built-in command");

    recipe_coordinate coordinate;
    recipe_plugin plugin;
    char err[256] = "";
    if(!plugin_read_recipe(name, &coordinate, &plugin, err, sizeof err)) {
        /* Not an error: a plugin being developed against PATH has no recipe,
           and saying what is not known is the honest report. */
        printf("  %-13s%s\n", "recipe", err);
        return exit_ok;
    }

    printf("  %-13s%s\n", "version", coordinate.version);
    printf("  %-13s%s\n", "target", coordinate.target);
    print_list("capabilities", plugin.capabilities, plugin.capability_count);
    print_list("extensions", plugin.extensions, plugin.extension_count);
    print_list("permissions", plugin.permissions, plugin.permission_count);
    if(plugin.ir_schema > 0)
        printf("  %-13s%ld\n", "ir schema", plugin.ir_schema);
    if(plugin.molto_min[0] != '\0')
        printf("  %-13s%s\n", "needs molto", plugin.molto_min);
    return exit_ok;
}

/* --- install --- */

/* Everything the person is being asked to agree to, before it happens. */
static void announce(const plugin_candidate *candidate) {
    printf("%s %s (%s)\n", candidate->coordinate.name, candidate->coordinate.version,
           candidate->coordinate.target);
    print_list("capabilities", candidate->plugin.capabilities, candidate->plugin.capability_count);
    print_list("permissions", candidate->plugin.permissions, candidate->plugin.permission_count);
}

/* The question, on stderr beside the answer it belongs to. Declining is not a
   failure: someone read what it asked for and said no. */
static bool confirm(void) {
    if(!progress_is_interactive(stdin)) {
        fprintf(stderr, "molto: refusing to install without a confirmation; pass --yes\n");
        return false;
    }
    fprintf(stderr, "\n  Install it? [Y/n] ");
    (void)fflush(stderr);

    char answer[16] = "";
    if(fgets(answer, sizeof answer, stdin) == NULL)
        return false;
    if(answer[0] == '\n' || answer[0] == '\0')
        return true;
    return answer[0] == 'y' || answer[0] == 'Y';
}

/* `<name>` or `<name>@<version>`, split in place. */
static const char *split_version(char *name) {
    char *at = strchr(name, '@');
    if(at == NULL)
        return NULL;
    *at = '\0';
    return at + 1;
}

static int run_install(const char *spec, const char *registry, bool assume_yes) {
    if(spec == NULL) {
        fprintf(stderr, "molto: plugin install needs a name\n");
        return exit_usage_error;
    }

    char name[PLUGIN_NAME_MAX];
    if(snprintf(name, sizeof name, "%s", spec) < 0 || strlen(spec) >= sizeof name) {
        fprintf(stderr, "molto: '%s' is not a plugin name\n", spec);
        return exit_usage_error;
    }
    const char *version = split_version(name);

    plugin_candidate candidate;
    char err[512] = "";
    if(!plugin_prepare(registry, name, version, &candidate, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_dependency_failure;
    }

    announce(&candidate);
    if(!assume_yes && !confirm()) {
        fprintf(stderr, "molto: nothing was installed\n");
        return exit_ok;
    }

    if(!plugin_install(&candidate, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_plugin_failure;
    }
    printf("Installed %s %s\n", candidate.coordinate.name, candidate.coordinate.version);
    return exit_ok;
}

/* --- remove --- */

static int run_remove(const char *name) {
    if(name == NULL) {
        fprintf(stderr, "molto: plugin remove needs a name\n");
        return exit_usage_error;
    }

    char err[512] = "";
    if(!plugin_remove(name, err, sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return exit_dependency_failure;
    }
    printf("Removed %s\n", name);
    return exit_ok;
}

/* --- entry point --- */

int plugin_command_run(const char *action, const char *name, const char *registry,
                       bool assume_yes) {
    if(action == NULL || strcmp(action, "list") == 0)
        return run_list();
    if(strcmp(action, "info") == 0)
        return run_info(name);
    if(strcmp(action, "install") == 0)
        return run_install(name, registry, assume_yes);
    if(strcmp(action, "remove") == 0)
        return run_remove(name);

    fprintf(stderr, "molto: '%s' is not a plugin action (list, info, install, remove)\n", action);
    return exit_usage_error;
}
