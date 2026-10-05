#include <molto/commands/new_command.h>

#include <molto/exit_code.h>
#include <molto/services/scaffold_service.h>

#include <stdio.h>

int new_command_run(const char *name, project_kind kind) {
    if(name == NULL || name[0] == '\0') {
        fprintf(stderr, "molto: 'new' requires a project name\n");
        return exit_usage_error;
    }
    /* Asked only for a library: a binary has no tests to pin anything for. */
    char moltest_tag[64] = "";
    if(kind == project_kind_library)
        scaffold_newest_moltest_tag(moltest_tag, sizeof moltest_tag);
    int code = scaffold_project(name, name, kind, moltest_tag);
    if(code == exit_ok)
        printf("Created %s '%s'\n", kind == project_kind_library ? "library" : "binary", name);
    return code;
}
