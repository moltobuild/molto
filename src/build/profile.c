#include <molto/build/profile.h>

#include <string.h>

typedef struct {
    const char *name;
    build_profile profile;
} profile_entry;

static const profile_entry profile_table[] = {
    {"debug", profile_debug},
    {"release", profile_release},
    {"bench", profile_bench},
    {"custom", profile_custom},
};

bool profile_parse(const char *name, build_profile *out) {
    if(name == NULL)
        return false;
    size_t count = sizeof profile_table / sizeof profile_table[0];
    for(size_t i = 0; i < count; i++) {
        if(strcmp(name, profile_table[i].name) == 0) {
            *out = profile_table[i].profile;
            return true;
        }
    }
    return false;
}

const char *profile_name(build_profile profile) {
    size_t count = sizeof profile_table / sizeof profile_table[0];
    for(size_t i = 0; i < count; i++) {
        if(profile_table[i].profile == profile)
            return profile_table[i].name;
    }
    return profile_table[0].name;
}
