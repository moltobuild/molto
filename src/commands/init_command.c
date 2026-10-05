#include <molto/commands/init_command.h>

#include <molto/services/fs_service.h>

#include <molto/exit_code.h>
#include <molto/services/scaffold_service.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

int init_command_run(project_kind kind) {
    char cwd[PATH_MAX];
    if(!fs_current_dir(cwd, sizeof cwd)) {
        fprintf(stderr, "molto: could not read current directory\n");
        return exit_build_failure;
    }
    const char *slash = strrchr(cwd, '/');
    const char *base = slash != NULL ? slash + 1 : cwd;
    char moltest_tag[64] = "";
    if(kind == project_kind_library)
        scaffold_newest_moltest_tag(moltest_tag, sizeof moltest_tag);
    int code = scaffold_project(".", base, kind, moltest_tag);
    if(code == exit_ok)
        printf("Initialized %s '%s'\n", kind == project_kind_library ? "library" : "binary", base);
    return code;
}
