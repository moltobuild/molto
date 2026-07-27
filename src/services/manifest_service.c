#include <molto/services/manifest_service.h>

#include <stdio.h>
#include <stdlib.h>

bool manifest_is_valid_name(const char *name) {
    if (name == NULL || name[0] == '\0')
        return false;
    if (!(name[0] >= 'a' && name[0] <= 'z'))
        return false;
    for (size_t i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        bool allowed = (c >= 'a' && c <= 'z')
                    || (c >= '0' && c <= '9')
                    || c == '_';
        if (!allowed)
            return false;
    }
    return true;
}

static const char manifest_template[] =
    "[package]\n"
    "name = \"%s\"\n"
    "version = \"0.1.0\"\n"
    "\n"
    "[profile.debug]\n"
    "opt_level = 0\n"
    "debug_info = true\n"
    "\n"
    "[profile.release]\n"
    "opt_level = 3\n"
    "debug_info = false\n";

char *manifest_render_default(const char *name) {
    if (!manifest_is_valid_name(name))
        return NULL;
    int needed = snprintf(NULL, 0, manifest_template, name);
    if (needed < 0)
        return NULL;
    size_t size = (size_t)needed + 1;
    char *buffer = malloc(size);
    if (buffer == NULL)
        return NULL;
    snprintf(buffer, size, manifest_template, name);
    return buffer;
}
