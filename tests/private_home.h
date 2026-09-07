#ifndef MOLTO_TESTS_PRIVATE_HOME_H
#define MOLTO_TESTS_PRIVATE_HOME_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Giving a test a molto home of its own takes two variables, not one.
 *
 * Four fixtures here redirect `$HOME` at a temporary directory and trust that
 * everything reading a molto path follows. That was true until `$MOLTO_HOME`
 * existed. `paths_molto_home` asks for the override first and answers with it
 * whole, so on a machine where it is set the sandbox is not consulted at all —
 * and a test that saves a credential saves it over the developer's own, while
 * the teardown deletes a file inside the sandbox that was never written. The
 * suite passes either way, which is what makes it worth stating here rather
 * than leaving to each fixture to remember.
 *
 * Cleared rather than pointed at the sandbox, because what these fixtures are
 * testing is the `$HOME` path: `$MOLTO_HOME` winning over it is its own
 * property, and it is tested where it belongs, in test_paths_service.c.
 */

#define PRIVATE_HOME_MAX 1024

typedef struct {
    char value[PRIVATE_HOME_MAX];
    bool was_set;
} molto_home_override;

/* Take `$MOLTO_HOME` out of the environment for the length of a test. */
static inline void molto_home_override_clear(molto_home_override *saved) {
    const char *value = getenv("MOLTO_HOME");
    saved->was_set = value != NULL;
    snprintf(saved->value, sizeof saved->value, "%s", value != NULL ? value : "");
    (void)unsetenv("MOLTO_HOME");
}

/* Put it back exactly as it was, including having been unset. */
static inline void molto_home_override_restore(const molto_home_override *saved) {
    if(saved->was_set)
        (void)setenv("MOLTO_HOME", saved->value, 1);
    else
        (void)unsetenv("MOLTO_HOME");
}

#endif /* MOLTO_TESTS_PRIVATE_HOME_H */
