#include <molto/services/manifest_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Trim leading/trailing whitespace in place and return the trimmed start. */
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t')
        text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t'
                       || end[-1] == '\r' || end[-1] == '\n'))
        end--;
    *end = '\0';
    return text;
}

/* Scan a TOML-subset string for `key` inside `[section]`.
   When found, copies the raw (untrimmed of quotes) value into `out`.
   A NULL `key` turns this into a section-existence probe. */
static bool manifest_lookup(const char *toml, const char *section,
                            const char *key, char *out, size_t out_size) {
    char line[512];
    char current[256] = "";
    bool in_section = false;
    const char *cursor = toml;
    while (*cursor != '\0') {
        size_t n = 0;
        while (*cursor != '\0' && *cursor != '\n' && n + 1 < sizeof line)
            line[n++] = *cursor++;
        line[n] = '\0';
        while (*cursor != '\0' && *cursor != '\n')
            cursor++;
        if (*cursor == '\n')
            cursor++;

        char *text = trim(line);
        if (text[0] == '\0' || text[0] == '#')
            continue;
        if (text[0] == '[') {
            char *close = strchr(text, ']');
            if (close == NULL)
                continue;
            *close = '\0';
            snprintf(current, sizeof current, "%s", text + 1);
            in_section = strcmp(current, section) == 0;
            if (in_section && key == NULL)
                return true;
            continue;
        }
        if (!in_section)
            continue;
        char *equals = strchr(text, '=');
        if (equals == NULL)
            continue;
        *equals = '\0';
        char *found_key = trim(text);
        char *value = trim(equals + 1);
        if (strcmp(found_key, key) == 0) {
            snprintf(out, out_size, "%s", value);
            return true;
        }
    }
    return false;
}

bool manifest_read_name(const char *toml, char *out, size_t out_size) {
    char raw[256];
    if (!manifest_lookup(toml, "package", "name", raw, sizeof raw))
        return false;
    char *value = raw;
    size_t length = strlen(value);
    if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
        value[length - 1] = '\0';
        value++;
    }
    if (!manifest_is_valid_name(value))
        return false;
    snprintf(out, out_size, "%s", value);
    return true;
}

bool manifest_read_profile(const char *toml, const char *name,
                           manifest_profile *out) {
    char section[256];
    snprintf(section, sizeof section, "profile.%s", name);
    if (!manifest_lookup(toml, section, NULL, NULL, 0))
        return false;
    char raw[64];
    if (manifest_lookup(toml, section, "opt_level", raw, sizeof raw))
        out->opt_level = (int)strtol(raw, NULL, 10);
    if (manifest_lookup(toml, section, "debug_info", raw, sizeof raw))
        out->debug_info = strcmp(raw, "true") == 0;
    return true;
}
