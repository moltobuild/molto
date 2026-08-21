#include <molto/util/cli.h>

#include <stdio.h>
#include <string.h>

/* Caps for a single command's parsed arguments (options are static tables). */
#define CLI_MAX_OPTIONS 32
#define CLI_MAX_POSITIONALS 32

/* Exit codes the framework itself returns (match RFC-0002). */
#define CLI_EXIT_OK 0
#define CLI_EXIT_USAGE 4

struct cli_args {
    const cli_command *command;
    bool opt_present[CLI_MAX_OPTIONS];
    const char *opt_value[CLI_MAX_OPTIONS];
    const char *positionals[CLI_MAX_POSITIONALS];
    size_t positional_count;
    char *const *forwarded;
    int forwarded_count;
};

/* --- lookup helpers --- */

static const cli_command *find_command(const cli_app *app, const char *name) {
    for(size_t i = 0; i < app->command_count; i++) {
        if(strcmp(app->commands[i].name, name) == 0)
            return &app->commands[i];
    }
    return NULL;
}

static const cli_option *find_long_option(const cli_command *cmd, const char *name,
                                          size_t name_len) {
    for(size_t i = 0; i < cmd->option_count; i++) {
        const char *candidate = cmd->options[i].long_name;
        if(strlen(candidate) == name_len && strncmp(candidate, name, name_len) == 0)
            return &cmd->options[i];
    }
    return NULL;
}

static const cli_option *find_short_option(const cli_command *cmd, char letter) {
    for(size_t i = 0; i < cmd->option_count; i++) {
        if(cmd->options[i].short_name == letter)
            return &cmd->options[i];
    }
    return NULL;
}

/* --- help rendering --- */

static void render_option(const cli_option *opt, FILE *stream) {
    char names[64];
    if(opt->short_name != 0)
        snprintf(names, sizeof names, "-%c, %s", opt->short_name, opt->long_name);
    else
        snprintf(names, sizeof names, "    %s", opt->long_name);
    char full[96];
    if(opt->kind == cli_opt_value && opt->value_name != NULL)
        snprintf(full, sizeof full, "%s %s", names, opt->value_name);
    else
        snprintf(full, sizeof full, "%s", names);
    fprintf(stream, "    %-28s %s", full, opt->help != NULL ? opt->help : "");
    if(opt->kind == cli_opt_value && opt->default_value != NULL)
        fprintf(stream, " (default: %s)", opt->default_value);
    fputc('\n', stream);
}

static void print_command_help(const cli_app *app, const cli_command *cmd, FILE *stream) {
    fprintf(stream, "Usage:\n    %s %s [options]%s%s\n\n", app->program, cmd->name,
            cmd->arg_name != NULL ? " " : "", cmd->arg_name != NULL ? cmd->arg_name : "");
    if(cmd->summary != NULL)
        fprintf(stream, "%s\n\n", cmd->summary);
    fprintf(stream, "Options:\n");
    for(size_t i = 0; i < cmd->option_count; i++)
        render_option(&cmd->options[i], stream);
    fprintf(stream, "    %-28s %s\n", "-h, --help", "Show this help");
}

static void print_global_help(const cli_app *app, FILE *stream) {
    fprintf(stream, "%s %s - %s\n\n", app->program, app->version, app->tagline);
    fprintf(stream, "Usage:\n    %s <command> [options]\n\n", app->program);
    fprintf(stream, "Commands:\n");
    for(size_t i = 0; i < app->command_count; i++)
        fprintf(stream, "    %-12s %s\n", app->commands[i].name,
                app->commands[i].summary != NULL ? app->commands[i].summary : "");
    fprintf(stream, "\nOptions:\n");
    fprintf(stream, "    %-12s %s\n", "-h, --help", "Show this help");
    fprintf(stream, "    %-12s %s\n", "-V, --version", "Show version");
    fprintf(stream, "\nRun '%s <command> --help' for command details.\n", app->program);
}

/* --- option matching (mutates `args`, advances *index as needed) --- */

/* Record a value/flag for `opt`, consuming the next token for a value option
   when `inline_value` is NULL. Returns false on a missing value. */
static bool apply_option(cli_args *args, const cli_option *opt, const char *inline_value, int argc,
                         char **argv, int *index) {
    size_t slot = (size_t)(opt - args->command->options);
    args->opt_present[slot] = true;
    if(opt->kind == cli_opt_value) {
        if(inline_value != NULL) {
            args->opt_value[slot] = inline_value;
        } else if(*index + 1 < argc) {
            args->opt_value[slot] = argv[++(*index)];
        } else {
            return false;
        }
    }
    return true;
}

static int usage_error(const cli_app *app, const cli_command *cmd, const char *message,
                       const char *detail) {
    fprintf(stderr, "%s: %s '%s'\n\n", app->program, message, detail);
    print_command_help(app, cmd, stderr);
    return CLI_EXIT_USAGE;
}

int cli_app_run(const cli_app *app, int argc, char **argv) {
    if(argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_global_help(app, stdout);
        return CLI_EXIT_OK;
    }
    if(strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("%s %s\n", app->program, app->version);
        return CLI_EXIT_OK;
    }

    const cli_command *cmd = find_command(app, argv[1]);
    if(cmd == NULL) {
        /* The table wins first, so nothing served here can shadow a built-in. */
        int handled = CLI_EXIT_OK;
        if(app->unknown != NULL && app->unknown(argv[1], argc - 2, argv + 2, &handled))
            return handled;

        fprintf(stderr, "%s: unknown command '%s'\n\n", app->program, argv[1]);
        print_global_help(app, stderr);
        return CLI_EXIT_USAGE;
    }

    cli_args args;
    memset(&args, 0, sizeof args);
    args.command = cmd;

    for(int i = 2; i < argc; i++) {
        const char *token = argv[i];

        if(strcmp(token, "--") == 0) {
            args.forwarded = &argv[i + 1];
            args.forwarded_count = argc - (i + 1);
            break;
        }
        if(strcmp(token, "--help") == 0 || strcmp(token, "-h") == 0) {
            print_command_help(app, cmd, stdout);
            return CLI_EXIT_OK;
        }

        if(token[0] == '-' && token[1] == '-') {
            const char *equals = strchr(token, '=');
            size_t name_len = equals != NULL ? (size_t)(equals - token) : strlen(token);
            const cli_option *opt = find_long_option(cmd, token, name_len);
            if(opt == NULL)
                return usage_error(app, cmd, "unknown option", token);
            const char *inline_value = equals != NULL ? equals + 1 : NULL;
            if(opt->kind == cli_opt_flag && inline_value != NULL)
                return usage_error(app, cmd, "option takes no value", token);
            if(!apply_option(&args, opt, inline_value, argc, argv, &i))
                return usage_error(app, cmd, "missing value for option", token);
        } else if(token[0] == '-' && token[1] != '\0') {
            const cli_option *opt = find_short_option(cmd, token[1]);
            if(opt == NULL)
                return usage_error(app, cmd, "unknown option", token);
            const char *inline_value = token[2] != '\0' ? &token[2] : NULL;
            if(opt->kind == cli_opt_flag && inline_value != NULL)
                return usage_error(app, cmd, "option takes no value", token);
            if(!apply_option(&args, opt, inline_value, argc, argv, &i))
                return usage_error(app, cmd, "missing value for option", token);
        } else {
            if(args.positional_count < CLI_MAX_POSITIONALS)
                args.positionals[args.positional_count++] = token;
        }
    }

    return cmd->handler(&args);
}

/* --- accessors --- */

static const cli_option *option_by_name(const cli_args *args, const char *long_name,
                                        size_t *slot_out) {
    const cli_command *cmd = args->command;
    for(size_t i = 0; i < cmd->option_count; i++) {
        if(strcmp(cmd->options[i].long_name, long_name) == 0) {
            *slot_out = i;
            return &cmd->options[i];
        }
    }
    return NULL;
}

const char *cli_args_option(const cli_args *args, const char *long_name) {
    size_t slot;
    const cli_option *opt = option_by_name(args, long_name, &slot);
    if(opt == NULL)
        return NULL;
    if(args->opt_present[slot])
        return args->opt_value[slot];
    return opt->default_value;
}

bool cli_args_flag(const cli_args *args, const char *long_name) {
    size_t slot;
    if(option_by_name(args, long_name, &slot) == NULL)
        return false;
    return args->opt_present[slot];
}

const char *cli_args_positional(const cli_args *args, size_t index) {
    if(index >= args->positional_count)
        return NULL;
    return args->positionals[index];
}

size_t cli_args_positional_count(const cli_args *args) { return args->positional_count; }

char *const *cli_args_forwarded(const cli_args *args, int *count) {
    *count = args->forwarded_count;
    return args->forwarded;
}

const char *cli_args_command_name(const cli_args *args) { return args->command->name; }
