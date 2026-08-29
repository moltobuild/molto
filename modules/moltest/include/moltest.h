#ifndef MOLTEST_H
#define MOLTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * What the platform does not provide, provided here once.
 *
 * A test framework is where this belongs: the suites that need a temporary
 * directory or an environment variable need them on every platform, and the
 * alternative is the same three shims repeated in every file that wants one.
 */
#ifdef _WIN32
#include <direct.h>
#include <io.h>

static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name) != NULL)
        return 0;
    return _putenv_s(name, value);
}

/* Windows removes a variable by assigning it nothing, which is the same thing
   said differently rather than an approximation. */
static inline int unsetenv(const char *name) {
    return _putenv_s(name, "");
}
#endif

/*
 * A temporary directory of one's own, created and named.
 *
 * `prefix` names the test rather than the path: the directory goes wherever
 * the platform keeps temporary files, and a suite that spelled `/tmp` itself
 * would be a suite that only runs on one of them. That is not hypothetical —
 * it is how every one of these was written before, and it is the single
 * largest reason the suites did not compile for Windows.
 *
 * False when the directory could not be made, which a caller should treat as
 * a failed fixture rather than as a test result.
 */
[[nodiscard]] static inline bool moltest_temp_dir(const char *prefix, char *out, size_t size) {
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (base == NULL)
        base = getenv("TMP");
    if (base == NULL)
        base = ".";
#else
    const char *base = "/tmp";
#endif
    const int written = snprintf(out, size, "%s/%s_XXXXXX", base, prefix);
    if (written < 0 || (size_t)written >= size)
        return false;
#ifdef _WIN32
    /* `_mktemp_s` only picks the name; the directory is still ours to make. */
    if (_mktemp_s(out, (size_t)written + 1) != 0)
        return false;
    return _mkdir(out) == 0;
#else
    return mkdtemp(out) != NULL;
#endif
}

/*
 * moltest — a small unit-testing framework for C, in the spirit of pytest.
 *
 * Tests register themselves, so there is no list to maintain:
 *
 *     #include <moltest.h>
 *
 *     MOLTEST(str_list_push_appends) {
 *         str_list list;
 *         str_list_init(&list);
 *         EXPECT_TRUE(str_list_push(&list, "a"));
 *         EXPECT_EQ(1, str_list_count(&list));
 *         str_list_free(&list);
 *     }
 *
 * EXPECT_* records a failure and lets the test continue; ASSERT_* also stops
 * the current test. SKIP()/WARN()/FAIL() control the outcome explicitly. The
 * runner groups results per file, shows progress with a percentage, and prints
 * detailed failures (expected, actual, location) and warnings.
 */

/* ------------------------------------------------------------------ */
/* Test registration                                                    */
/* ------------------------------------------------------------------ */

typedef void (*moltest_fn)(void);

/* Add a test to the registry. Called automatically by the MOLTEST macros;
   `skip_reason` is NULL for a normal test. */
void moltest_register(const char *name, const char *file, moltest_fn fn,
                      const char *skip_reason);

/* Define a test and register it before main() runs. */
#define MOLTEST(test_name)                                                     \
    static void moltest_case_##test_name(void);                                \
    __attribute__((constructor))                                               \
    static void moltest_register_##test_name(void) {                           \
        moltest_register(#test_name, __FILE__, moltest_case_##test_name, NULL);\
    }                                                                          \
    static void moltest_case_##test_name(void)

/* Define a test that is reported as skipped without running. */
#define MOLTEST_SKIP(test_name, reason)                                        \
    static void moltest_case_##test_name(void);                                \
    __attribute__((constructor))                                               \
    static void moltest_register_##test_name(void) {                           \
        moltest_register(#test_name, __FILE__, moltest_case_##test_name,       \
                         (reason));                                            \
    }                                                                          \
    static void moltest_case_##test_name(void)

/* ------------------------------------------------------------------ */
/* Assertion plumbing (used by the macros below)                        */
/* ------------------------------------------------------------------ */

bool moltest_check_bool(bool value, bool expected, const char *expr,
                        const char *file, int line);
bool moltest_check_int(long long actual, long long expected, bool want_equal,
                       const char *expr, const char *file, int line);
bool moltest_check_str(const char *actual, const char *expected, bool want_equal,
                       const char *expr, const char *file, int line);
bool moltest_check_ptr(const void *actual, const void *expected, bool want_equal,
                       const char *expr, const char *file, int line);
bool moltest_check_null(const void *value, bool want_null, const char *expr,
                        const char *file, int line);
/* `op` is one of "<", "<=", ">", ">=" */
bool moltest_check_order(long long lhs, long long rhs, const char *op,
                         const char *expr, const char *file, int line);

void moltest_record_warning(const char *message, const char *file, int line);
void moltest_record_skip(const char *reason, const char *file, int line);
void moltest_record_failure(const char *message, const char *file, int line);

/* ------------------------------------------------------------------ */
/* Assertions                                                           */
/* ------------------------------------------------------------------ */

#define EXPECT_TRUE(expr) \
    ((void)moltest_check_bool((expr) ? true : false, true, #expr, __FILE__, __LINE__))
#define EXPECT_FALSE(expr) \
    ((void)moltest_check_bool((expr) ? true : false, false, #expr, __FILE__, __LINE__))

/* EXPECT_EQ/NE dispatch on the type of `actual`: strings compare with strcmp,
   everything else compares as an integer. Use EXPECT_STREQ or EXPECT_PTR_EQ
   explicitly when you want to be unambiguous (e.g. for struct pointers). */
#define MOLTEST_EQ_FN(actual) _Generic((actual),                                \
        char *: moltest_check_str,                                             \
        const char *: moltest_check_str,                                       \
        default: moltest_check_int)

#define EXPECT_EQ(expected, actual) \
    ((void)MOLTEST_EQ_FN(actual)((actual), (expected), true, #actual " == " #expected, __FILE__, __LINE__))
#define EXPECT_NE(expected, actual) \
    ((void)MOLTEST_EQ_FN(actual)((actual), (expected), false, #actual " != " #expected, __FILE__, __LINE__))
/* Alias for readability. */
#define EXPECT_EQUALS(expected, actual) EXPECT_EQ(expected, actual)

#define EXPECT_STREQ(expected, actual) \
    ((void)moltest_check_str((actual), (expected), true, #actual " == " #expected, __FILE__, __LINE__))
#define EXPECT_STRNE(expected, actual) \
    ((void)moltest_check_str((actual), (expected), false, #actual " != " #expected, __FILE__, __LINE__))

#define EXPECT_PTR_EQ(expected, actual) \
    ((void)moltest_check_ptr((actual), (expected), true, #actual " == " #expected, __FILE__, __LINE__))
#define EXPECT_PTR_NE(expected, actual) \
    ((void)moltest_check_ptr((actual), (expected), false, #actual " != " #expected, __FILE__, __LINE__))

#define EXPECT_NULL(value) \
    ((void)moltest_check_null((value), true, #value, __FILE__, __LINE__))
#define EXPECT_NOT_NULL(value) \
    ((void)moltest_check_null((value), false, #value, __FILE__, __LINE__))

#define EXPECT_LT(lhs, rhs) \
    ((void)moltest_check_order((lhs), (rhs), "<", #lhs " < " #rhs, __FILE__, __LINE__))
#define EXPECT_LE(lhs, rhs) \
    ((void)moltest_check_order((lhs), (rhs), "<=", #lhs " <= " #rhs, __FILE__, __LINE__))
#define EXPECT_GT(lhs, rhs) \
    ((void)moltest_check_order((lhs), (rhs), ">", #lhs " > " #rhs, __FILE__, __LINE__))
#define EXPECT_GE(lhs, rhs) \
    ((void)moltest_check_order((lhs), (rhs), ">=", #lhs " >= " #rhs, __FILE__, __LINE__))

/* ASSERT_* variants stop the current test on failure. */
#define ASSERT_TRUE(expr)                                                      \
    do { if (!moltest_check_bool((expr) ? true : false, true, #expr,           \
                                 __FILE__, __LINE__)) return; } while (0)
#define ASSERT_FALSE(expr)                                                     \
    do { if (!moltest_check_bool((expr) ? true : false, false, #expr,          \
                                 __FILE__, __LINE__)) return; } while (0)
#define ASSERT_EQ(expected, actual)                                            \
    do { if (!MOLTEST_EQ_FN(actual)((actual), (expected), true,                \
                                    #actual " == " #expected, __FILE__, __LINE__)) return; } while (0)
#define ASSERT_STREQ(expected, actual)                                         \
    do { if (!moltest_check_str((actual), (expected), true,                    \
                                #actual " == " #expected, __FILE__, __LINE__)) return; } while (0)
#define ASSERT_NOT_NULL(value)                                                 \
    do { if (!moltest_check_null((value), false, #value,                       \
                                 __FILE__, __LINE__)) return; } while (0)
#define ASSERT_NULL(value)                                                     \
    do { if (!moltest_check_null((value), true, #value,                        \
                                 __FILE__, __LINE__)) return; } while (0)

/* ------------------------------------------------------------------ */
/* Outcome control                                                      */
/* ------------------------------------------------------------------ */

/* Mark the current test as skipped and stop it. */
#define SKIP(reason) \
    do { moltest_record_skip((reason), __FILE__, __LINE__); return; } while (0)
/* Record a warning (reported with its location) and keep going. */
#define WARN(message) moltest_record_warning((message), __FILE__, __LINE__)
/* Fail the current test explicitly and stop it. */
#define FAIL(message) \
    do { moltest_record_failure((message), __FILE__, __LINE__); return; } while (0)

/* ------------------------------------------------------------------ */
/* Runner and reporting                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    moltest_status_passed,
    moltest_status_failed,
    moltest_status_skipped,
    moltest_status_warned, /* passed, but produced warnings */
} moltest_status;

typedef struct {
    size_t files;
    size_t tests;
    size_t passed;
    size_t failed;
    size_t skipped;
    size_t warned;
    size_t assertions;
    size_t warnings;
    double seconds;
} moltest_summary;

/* Observer of a run. Every callback may be NULL. Set one to add an alternative
   output (JUnit XML, coverage, ...) without touching the runner. */
typedef struct {
    void (*on_run_start)(size_t files, size_t tests, void *ctx);
    void (*on_file_start)(const char *file, void *ctx);
    void (*on_test_end)(const char *file, const char *name, moltest_status status,
                        double seconds, void *ctx);
    void (*on_file_end)(const char *file, size_t done, size_t total, void *ctx);
    void (*on_run_end)(const moltest_summary *summary, void *ctx);
    void *ctx;
} moltest_reporter;

/* Install an additional reporter (the console one always runs). */
void moltest_set_reporter(const moltest_reporter *reporter);

/* Run the registered tests. Returns 0 when nothing failed, 1 otherwise.
   Recognised arguments: -k <substring>, -v, --list, --color=auto|always|never,
   -h/--help. */
[[nodiscard]] int moltest_run(int argc, char **argv);

#endif /* MOLTEST_H */
