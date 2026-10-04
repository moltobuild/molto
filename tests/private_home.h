#ifndef MOLTO_TESTS_PRIVATE_HOME_H
#define MOLTO_TESTS_PRIVATE_HOME_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Giving a test a molto home of its own takes more than one variable.
 *
 * Four fixtures here redirect `$HOME` at a temporary directory and trust that
 * everything reading a molto path follows. That was true until `$MOLTO_HOME`
 * existed. `paths_molto_config_dir` and its siblings ask for the override first
 * and answer with it whole, so on a machine where it is set the sandbox is not
 * consulted at all — and a test that saves a credential saves it over the
 * developer's own, while the teardown deletes a file inside the sandbox that
 * was never written. The suite passes either way, which is what makes it worth
 * stating here rather than leaving to each fixture to remember.
 *
 * The XDG variables are the same trap one level down: a developer who sets
 * `$XDG_CONFIG_HOME` would have the sandbox's credential written into their
 * real configuration. And on Windows the directories hang off `%APPDATA%`
 * rather than the home, so the sandbox has to name that one too.
 *
 * Cleared rather than pointed at the sandbox, because what these fixtures are
 * testing is the `$HOME` path: `$MOLTO_HOME` winning over it is its own
 * property, and it is tested where it belongs, in test_paths_service.c.
 */

#define PRIVATE_HOME_MAX 1024

/* Where the directories land, relative to a sandboxed home, once
   `private_home_point` has run. */
#ifdef _WIN32
#define PRIVATE_HOME_CONFIG "/AppData/Roaming/molto"
#define PRIVATE_HOME_DATA "/AppData/Roaming/molto"
#else
#define PRIVATE_HOME_CONFIG "/.config/molto"
#define PRIVATE_HOME_DATA "/.local/share/molto"
#endif

/* Every variable that can move a molto directory away from `$HOME`. */
static const char *const private_home_variables[] = {
    "MOLTO_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
#ifdef _WIN32
    "APPDATA",
#endif
};

#define PRIVATE_HOME_VARIABLES (sizeof private_home_variables / sizeof private_home_variables[0])

typedef struct {
    char value[PRIVATE_HOME_VARIABLES][PRIVATE_HOME_MAX];
    bool was_set[PRIVATE_HOME_VARIABLES];
} molto_home_override;

/* Take them out of the environment for the length of a test. */
static inline void molto_home_override_clear(molto_home_override *saved) {
    for(size_t i = 0; i < PRIVATE_HOME_VARIABLES; i++) {
        const char *value = getenv(private_home_variables[i]);
        saved->was_set[i] = value != NULL;
        snprintf(saved->value[i], sizeof saved->value[i], "%s", value != NULL ? value : "");
        (void)unsetenv(private_home_variables[i]);
    }
}

/* Put them back exactly as they were, including having been unset. */
static inline void molto_home_override_restore(const molto_home_override *saved) {
    for(size_t i = 0; i < PRIVATE_HOME_VARIABLES; i++) {
        if(saved->was_set[i])
            (void)setenv(private_home_variables[i], saved->value[i], 1);
        else
            (void)unsetenv(private_home_variables[i]);
    }
}

/* Point `$HOME` at `home`, and on Windows `%APPDATA%` at the place a profile
   keeps it, so every molto directory lands inside the sandbox. */
static inline bool private_home_point(const char *home) {
#ifdef _WIN32
    char appdata[PRIVATE_HOME_MAX];
    snprintf(appdata, sizeof appdata, "%s/AppData/Roaming", home);
    if(setenv("APPDATA", appdata, 1) != 0)
        return false;
#endif
    return setenv("HOME", home, 1) == 0;
}

#endif /* MOLTO_TESTS_PRIVATE_HOME_H */
