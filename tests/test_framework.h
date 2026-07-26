#ifndef MOLTO_TEST_FRAMEWORK_H
#define MOLTO_TEST_FRAMEWORK_H

#include <stdio.h>

extern int tests_run;
extern int tests_failed;

/* Assert a condition, recording the result in the global counters. */
#define CHECK(cond)                                                     \
    do {                                                                \
        tests_run++;                                                    \
        if (!(cond)) {                                                  \
            tests_failed++;                                             \
            printf("    FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                               \
    } while (0)

/* Run a named test suite function. */
#define RUN_SUITE(suite)          \
    do {                          \
        printf("- %s\n", #suite); \
        suite();                  \
    } while (0)

#endif /* MOLTO_TEST_FRAMEWORK_H */
