#ifndef MOLTEST_H
#define MOLTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#else
#include <unistd.h>
#endif
#ifdef _WIN32

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

/* Room for a path this harness hands back.
 *
 * A caller needs a buffer before it can be given a directory, and the size is
 * the harness's to know: what goes in it is a temporary root whose length
 * depends on where the platform keeps them. */
#define MOLTEST_PATH 256

/*
 * Where the platform keeps temporary files, in one separator.
 *
 * Windows hands back `C:\users\someone\Temp` and the helpers below then join
 * a name onto it with a slash, which produces a path spelled two ways at once.
 * That is not cosmetic: a suite writes these paths into files it then asks the
 * code under test to read, and in a TOML string a backslash begins an escape —
 * `"C:\users\..."` is a manifest no parser will take, and the failure arrives
 * as "the manifest is invalid" with nothing pointing at the path.
 *
 * Converted here because this is the boundary the path comes in through.
 * Windows accepts a forward slash everywhere it accepts a backslash. POSIX
 * converts nothing, and must not: a backslash is a legal character in a name.
 */
static inline void moltest_temp_base(char *out, size_t size, const char *prefix) {
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
    if (written < 0 || (size_t)written >= size) {
        out[0] = '\0';
        return;
    }
#ifdef _WIN32
    for (char *c = out; *c != '\0'; c++) {
        if (*c == '\\')
            *c = '/';
    }
#endif
}

/* A temporary file of one's own, created empty and named.
 *
 * The companion of moltest_temp_dir, and here for the same reason: a suite that
 * spells `/tmp` itself is a suite that only runs on one platform. */
/* `/` throughout, whatever the platform spells it. `TEMP` on Windows arrives
   with backslashes, and a fixture that hands one out is a fixture whose paths
   do not compare equal to the ones the code under test produces — which shows
   up as a test failing about a library nobody can find rather than about a
   separator. Win32 takes `/` in any case. */
static inline void moltest_one_separator(char *path) {
    for (char *at = path; *at != '\0'; at++) {
        if (*at == '\\')
            *at = '/';
    }
}

[[nodiscard]] static inline bool moltest_temp_file(const char *prefix, char *out, size_t size);

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
    moltest_temp_base(out, size, prefix);
    if (out[0] == '\0')
        return false;
#ifdef _WIN32
    /* `_mktemp_s` only picks the name; the directory is still ours to make. */
    if (_mktemp_s(out, strlen(out) + 1) != 0)
        return false;
    if (_mkdir(out) != 0)
        return false;
    moltest_one_separator(out);
    return true;
#else
    return mkdtemp(out) != NULL;
#endif
}

static inline bool moltest_temp_file(const char *prefix, char *out, size_t size) {
    moltest_temp_base(out, size, prefix);
    if (out[0] == '\0')
        return false;
#ifdef _WIN32
    if (_mktemp_s(out, strlen(out) + 1) != 0)
        return false;
    moltest_one_separator(out);
    FILE *file = fopen(out, "wb");
    if (file == NULL)
        return false;
    (void)fclose(file);
    return true;
#else
    const int fd = mkstemp(out);
    if (fd < 0)
        return false;
    (void)close(fd);
    return true;
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

/*
 * Define a behaviour a fabricated program can be given, and register it before
 * main() runs.
 *
 * The `out`/`exit` directives cover a fake that prints a version and leaves. A
 * fake standing in for a compiler does not: it reads its arguments, and what it
 * does depends on them. Rather than grow the spec into a small shell, the
 * behaviour is written in C beside the tests that rely on it, and the spec
 * names it — `behave <name>`.
 *
 * The body receives `argc` and `argv` as the fake was started with, and returns
 * the status to exit with.
 */
#define MOLTEST_FAKE(fake_name)                                                \
    static int moltest_fake_body_##fake_name(int argc, char **argv);           \
    __attribute__((constructor))                                               \
    static void moltest_register_fake_##fake_name(void) {                      \
        moltest_register_fake(#fake_name, moltest_fake_body_##fake_name);      \
    }                                                                          \
    static int moltest_fake_body_##fake_name(int argc, char **argv)

/* What a registered behaviour looks like. */
typedef int (*moltest_fake_fn)(int argc, char **argv);

void moltest_register_fake(const char *name, moltest_fake_fn body);

/* What a `set` line in this program's spec said, or NULL.
 *
 * A behaviour receives only the arguments it was started with, and a fake
 * compiler needs more than that — which library it ships, which flags it will
 * serve. Those are configuration, not arguments, so they travel in the spec
 * and are read back here. Only meaningful inside a MOLTEST_FAKE body. */
[[nodiscard]] const char *moltest_fake_setting(const char *key);

/* What this program was fed on stdin, NUL-terminated and never NULL.
 *
 * A compiler decides by the source it was handed — a program that includes
 * nothing needs no standard library — so a fake standing in for one has to be
 * able to read it. Only meaningful inside a MOLTEST_FAKE body. */
[[nodiscard]] const char *moltest_fake_input(void);

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

/*
 * Fabricate a program that behaves as `spec` says.
 *
 * A suite that needs a fake compiler used to write `#!/bin/sh` into a file and
 * call chmod. That is not a program on Windows — there is no shebang for the
 * process API to honour — so the fake was a file nothing could run, and forty
 * tests went down with it.
 *
 * What is planted instead is a **copy of the running test binary**, with the
 * spec written beside it. Started, it reads that spec, does as it says and
 * exits before a single test is registered. No compiler is needed at test time
 * and there is one mechanism on both platforms, so a fake behaves the same way
 * wherever the suite runs.
 *
 * `spec` is one directive per line:
 *
 *     out <text>    print to stdout, with a newline
 *     err <text>    print to stderr, with a newline
 *     exit <n>      leave with this status (default 0)
 *     link          honour `-o <path>` by putting a runnable program there
 *     set <k> <v>   a setting the behaviour can read back
 *     behave <name> hand over to a behaviour defined with MOLTEST_FAKE
 *     exec <program> become that program, arguments and streams intact
 *
 * `link` is what lets a fake stand in for a compiler: a health probe links a
 * program and then runs it, so the output has to be something the platform can
 * start. What lands there is another copy of this binary, told to exit 0.
 *
 * The platform's executable suffix is appended, because on Windows the suffix
 * is what makes a file runnable. `made`, when not NULL, receives the path the
 * program actually landed at.
 */
[[nodiscard]] bool moltest_fake_program(const char *path, const char *spec, char *made,
                                        size_t size);

/*
 * Append `line`, and a newline, to `path` as one operation.
 *
 * For the log a fake keeps of what it was called with, which is often not a
 * file it has to itself: `lint` runs a compiler pass and a linter pass over
 * the same source at the same time, and both fakes write to the same log.
 *
 * `fopen(path, "ab")` does not survive that on Windows. Its C runtime spells
 * append as a seek to the end followed by a write, and between the two the
 * other process seeks to the same offset — so one line lands on top of the
 * other and a fake that ran leaves no trace that it did. POSIX places the
 * write at the end under one lock and has no such gap.
 *
 * False when the line could not be written, which a fake should treat the way
 * it treats any other failed write.
 */
[[nodiscard]] bool moltest_append_line(const char *path, const char *line);

/*
 * Record what a fake was called with: `prefix`, when given, then `argv[1]`
 * onward separated by spaces, as one line in `path`.
 *
 * The line is measured before it is built, so a compile line of any length is
 * recorded whole — an assertion about the last flag on it would otherwise be
 * an assertion about a buffer size.
 */
[[nodiscard]] bool moltest_log_argv(const char *path, const char *prefix, int argc, char **argv);

#endif /* MOLTEST_H */
