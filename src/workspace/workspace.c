#include <molto/workspace/workspace.h>

#include <molto/services/fs_service.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool workspace_find_root(char *out, size_t out_size) {
    char dir[PATH_MAX];
    if(getcwd(dir, sizeof dir) == NULL)
        return false;

    for(;;) {
        char manifest[PATH_MAX + 16];
        snprintf(manifest, sizeof manifest, "%s/Project.toml", dir);
        if(fs_path_exists(manifest)) {
            snprintf(out, out_size, "%s", dir);
            return true;
        }
        /* Move to the parent, stopping once we pass the filesystem root. */
        if(dir[0] == '/' && dir[1] == '\0')
            return false;
        char *slash = strrchr(dir, '/');
        if(slash == NULL)
            return false;
        if(slash == dir)
            dir[1] = '\0'; /* parent of "/foo" is "/" */
        else
            *slash = '\0';
    }
}
