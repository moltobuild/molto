#include <molto/services/manifest_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool manifest_is_valid_name(const char *name) {
    if(name == NULL || name[0] == '\0')
        return false;
    if(!(name[0] >= 'a' && name[0] <= 'z'))
        return false;
    for(size_t i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if(!allowed)
            return false;
    }
    return true;
}

/* The range operators. Two-character ones come first so that at a given
   position ">=" is reported as ">=" and not as ">". Whitespace is one of them:
   "1.0.0 2.0.0" is a conjunction with the comma left out. */
static const char *const RANGE_OPERATORS[] = {">=", "<=", "^", "~", ">", "<",
                                              "*",  ",",  "=", " ", "\t"};

static void clear_operator(char *out, size_t size) {
    if(out != NULL && size > 0)
        out[0] = '\0';
}

/* The range operator `version` carries, or NULL.

   Scanned left to right rather than operator by operator, so what is reported
   is the first thing wrong with the string: "1.0.0, <2.0.0" is a conjunction
   whose comma comes before its '<', and naming the '<' would point past the
   actual mistake. */
static const char *range_operator_in(const char *version) {
    for(const char *p = version; *p != '\0'; p++) {
        for(size_t i = 0; i < sizeof RANGE_OPERATORS / sizeof RANGE_OPERATORS[0]; i++) {
            const char *candidate = RANGE_OPERATORS[i];
            if(strncmp(p, candidate, strlen(candidate)) == 0)
                return candidate;
        }
    }
    return NULL;
}

/* One or more digits, ending at `stop`. */
static bool is_number(const char *text, const char *stop) {
    if(text == stop)
        return false;
    for(const char *p = text; p < stop; p++) {
        if(*p < '0' || *p > '9')
            return false;
    }
    return true;
}

/* MAJOR.MINOR.PATCH, with the optional -prerelease and +build tails left
   unexamined beyond being non-empty: semver's job here is to order releases and
   to catch a typo, not to gate resolution (RFC-0008). */
static bool is_semver(const char *version) {
    const char *tail = version + strcspn(version, "-+");
    if(*tail != '\0' && tail[1] == '\0')
        return false; /* a "-" or "+" introducing nothing */

    const char *major = version;
    const char *first_dot = memchr(major, '.', (size_t)(tail - major));
    if(first_dot == NULL)
        return false;
    const char *minor = first_dot + 1;
    const char *second_dot = memchr(minor, '.', (size_t)(tail - minor));
    if(second_dot == NULL)
        return false;

    return is_number(major, first_dot) && is_number(minor, second_dot) &&
           is_number(second_dot + 1, tail);
}

bool manifest_is_exact_version(const char *version, char *out_operator, size_t operator_size) {
    clear_operator(out_operator, operator_size);
    if(version == NULL || version[0] == '\0')
        return false;

    const char *found = range_operator_in(version);
    if(found != NULL) {
        if(out_operator != NULL && operator_size > 0)
            snprintf(out_operator, operator_size, "%s", found);
        return false;
    }
    return is_semver(version);
}

/* The starter manifest. `std` is declared rather than left to the compiler's
   default, which varies by toolchain and version, so the project picks its own
   language standard instead of inheriting whatever the machine happens to have.
   `include` is declared too: headers under include/ are the normal layout for a
   C project, and a key that ships commented out teaches nothing — the build
   fails on the first #include and the explanation is a line the user never
   read. Scaffolding creates the directory, so the manifest never points at
   something absent. The remaining [target] keys stay commented: they document
   what can be set without adding settings the project did not ask for. */
static const char manifest_template[] =
    "[package]\n"
    "name = \"%s\"\n"
    "version = \"0.1.0\"\n"
    "\n"
    "[target]\n"
    "std = \"c17\"            # C standard passed as -std=\n"
    "include = [\"include\"]  # -Iinclude: the project's own headers\n"
    "# compiler = \"gcc\"     # gcc | clang | msvc; absent = autodetect\n"
    "# cpp_std = \"c++17\"    # C++ standard for C++ translation units\n"
    "# link = [\"m\"]         # system libraries: -lm\n"
    "# defines = []         # -D...\n"
    "# flags = []           # passed verbatim to the compiler and the linker\n"
    "\n"
    "[profile.debug]\n"
    "opt_level = 0\n"
    "debug_info = true\n"
    "\n"
    "[profile.release]\n"
    "opt_level = 3\n"
    "debug_info = false\n";

char *manifest_render_default(const char *name) {
    if(!manifest_is_valid_name(name))
        return NULL;
    int needed = snprintf(NULL, 0, manifest_template, name);
    if(needed < 0)
        return NULL;
    size_t size = (size_t)needed + 1;
    char *buffer = malloc(size);
    if(buffer == NULL)
        return NULL;
    snprintf(buffer, size, manifest_template, name);
    return buffer;
}
