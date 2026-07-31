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

/* The starter manifest. `std` is declared rather than left to the compiler's
   default, which varies by toolchain and version, so the project picks its own
   language standard instead of inheriting whatever the machine happens to have.
   The remaining [target] keys are commented out: they document what can be set
   without adding settings the project did not ask for. */
static const char manifest_template[] =
    "[package]\n"
    "name = \"%s\"\n"
    "version = \"0.1.0\"\n"
    "\n"
    "[target]\n"
    "std = \"c17\"          # C standard passed as -std=\n"
    "# compiler = \"gcc\"   # gcc | clang | msvc; absent = autodetect\n"
    "# cpp_std = \"c++17\"  # C++ standard for C++ translation units\n"
    "# link = [\"m\"]       # system libraries: -lm\n"
    "# defines = []       # -D...\n"
    "# include = []       # -I...\n"
    "# flags = []         # passed verbatim to the compiler and the linker\n"
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
