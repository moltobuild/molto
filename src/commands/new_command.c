#include <molto/commands/new_command.h>

#include <molto/exit_code.h>
#include <molto/services/scaffold_service.h>

#include <stdio.h>

int new_command_run(const char *name) {
    if(name == NULL || name[0] == '\0') {
        fprintf(stderr, "molto: 'new' requires a project name\n");
        return exit_usage_error;
    }
    int code = scaffold_project(name, name);
    if(code == exit_ok)
        printf("Created project '%s'\n", name);
    return code;
}
