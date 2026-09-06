#include <molto/services/paths_service.h>

#include <molto/services/fs_service.h>

#include <stdlib.h>

/* Pointed somewhere else entirely, when the default will not do. */
#define MOLTO_HOME_ENV "MOLTO_HOME"

/* What the directory is called inside a home. */
#define MOLTO_DIRNAME ".molto"

bool paths_molto_home(char *out, size_t size) {
    const char *override = getenv(MOLTO_HOME_ENV);
    if(override != NULL && override[0] != '\0')
        return fs_format_path(out, size, "%s", override);

    const char *home = getenv("HOME");
#ifdef _WIN32
    /* Windows names it differently, and the shells it ships with set only the
       other one. Asked in this order rather than the reverse because a shell
       that defines HOME on Windows means it: MSYS2 and git-bash point it at a
       home of their own choosing, and a tool run from there should agree with
       everything else run from there. */
    if(home == NULL || home[0] == '\0')
        home = getenv("USERPROFILE");
#endif
    if(home == NULL || home[0] == '\0')
        return false;
    return fs_format_path(out, size, "%s/%s", home, MOLTO_DIRNAME);
}

bool paths_molto_subdir(const char *subdirectory, char *out, size_t size) {
    char home[MOLTO_HOME_PATH_MAX];
    if(!paths_molto_home(home, sizeof home))
        return false;
    return fs_format_path(out, size, "%s/%s", home, subdirectory);
}
