#include <molto/build/profile.h>

#include <string.h>

typedef struct {
    const char *name;
    build_profile profile;
    manifest_profile defaults;
} profile_entry;

static const profile_entry profile_table[] = {
    { "debug",   profile_debug,   { .opt_level = 0, .debug_info = true } },
    { "release", profile_release, { .opt_level = 3, .debug_info = false } },
    { "bench",   profile_bench,   { .opt_level = 3, .debug_info = false } },
    { "custom",  profile_custom,  { .opt_level = 2, .debug_info = true } },
};

static const profile_entry *find_entry(build_profile profile) {
    size_t count = sizeof profile_table / sizeof profile_table[0];
    for (size_t i = 0; i < count; i++) {
        if (profile_table[i].profile == profile)
            return &profile_table[i];
    }
    return &profile_table[0];
}

bool profile_parse(const char *name, build_profile *out) {
    if (name == NULL)
        return false;
    size_t count = sizeof profile_table / sizeof profile_table[0];
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, profile_table[i].name) == 0) {
            *out = profile_table[i].profile;
            return true;
        }
    }
    return false;
}

const char *profile_name(build_profile profile) {
    return find_entry(profile)->name;
}

manifest_profile profile_defaults(build_profile profile) {
    return find_entry(profile)->defaults;
}
