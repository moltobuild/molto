#include <molto/commands/clean_command.h>

#include <molto/exit_code.h>
#include <molto/services/fs_service.h>
#include <molto/workspace/workspace.h>

#include <stdio.h>

/* Directories Molto owns inside a workspace, and may therefore delete. */
#define DIR_BUILD "build"
#define DIR_BIN   ".bin"

/* Size of the buffers holding the workspace root and the paths under it. */
#define CLEAN_PATH_SIZE 4096

/* Delete one of Molto's directories, reporting what was removed. A directory
   that is not there is not an error: the point is to end up without it. */
static bool remove_owned_dir(const char *root, const char *name) {
    char path[CLEAN_PATH_SIZE];
    if (!fs_format_path(path, sizeof path, "%s/%s", root, name)) {
        fprintf(stderr, "molto: path too long to compose (%s)\n", name);
        return false;
    }
    if (!fs_path_exists(path))
        return true;
    if (!fs_remove_tree(path)) {
        fprintf(stderr, "molto: could not remove '%s'\n", path);
        return false;
    }
    printf("Removed %s\n", name);
    return true;
}

int clean_command_run(bool all) {
    char root[CLEAN_PATH_SIZE];
    if (!workspace_find_root(root, sizeof root)) {
        fprintf(stderr, "molto: not inside a molto workspace (no Project.toml found)\n");
        return exit_invalid_manifest;
    }

    bool ok = remove_owned_dir(root, DIR_BUILD);
    /* `.bin/` holds the incremental state, not build output: removing it only
       costs a full rebuild, so it takes an explicit --all. */
    if (all)
        ok = remove_owned_dir(root, DIR_BIN) && ok;
    return ok ? exit_ok : exit_build_failure;
}
