#include <moltest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MOLTEST_VERSION "0.1.0"

/* Column where the per-file progress percentage is printed. */
#define PROGRESS_COLUMN 58
/* Buffer used to render a single expected/actual value. */
#define VALUE_BUFFER 256
/* Most captured output kept per test, shown only when the test fails. */
#define CAPTURE_LIMIT (16 * 1024)

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static char *dup_string(const char *text) {
    if (text == NULL)
        return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL)
        memcpy(copy, text, size);
    return copy;
}

/* Grow `*items` (an array of `element_size` elements) to hold one more. */
static bool ensure_capacity(void **items, size_t count, size_t *capacity,
                            size_t element_size) {
    if (count < *capacity)
        return true;
    size_t next = *capacity == 0 ? 16 : *capacity * 2;
    void *grown = realloc(*items, next * element_size);
    if (grown == NULL)
        return false;
    *items = grown;
    *capacity = next;
    return true;
}

/* ------------------------------------------------------------------ */
/* Registry and results                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *file;
    moltest_fn fn;
    const char *skip_reason; /* non-NULL: registered as skipped */
    moltest_status status;
    double seconds;
    size_t assertions;
    bool selected;
    char *output; /* what the test printed, kept for the failure report */
} test_entry;

typedef struct {
    size_t test;      /* index into `tests` */
    char *check;      /* the expression, or NULL for an explicit FAIL */
    char *expected;   /* rendered expected value, or NULL */
    char *actual;     /* rendered actual value, or NULL */
    char *message;    /* explicit message, or NULL */
    const char *file;
    int line;
} failure_record;

typedef struct {
    size_t test;
    char *message;
    const char *file;
    int line;
} warning_record;

static test_entry *tests;
static size_t test_count;
static size_t test_capacity;

static failure_record *failures;
static size_t failure_count;
static size_t failure_capacity;

static warning_record *warnings;
static size_t warning_count;
static size_t warning_capacity;

/* State of the test being executed. */
static size_t current_test;
static bool current_failed;
static bool current_skipped;
static size_t current_warnings;
static size_t total_assertions;

static const moltest_reporter *extra_reporter;

void moltest_register(const char *name, const char *file, moltest_fn fn,
                      const char *skip_reason) {
    if (!ensure_capacity((void **)&tests, test_count, &test_capacity, sizeof *tests))
        return;
    tests[test_count] = (test_entry){
        .name = name,
        .file = file,
        .fn = fn,
        .skip_reason = skip_reason,
        .status = moltest_status_passed,
        .selected = true,
    };
    test_count++;
}

void moltest_set_reporter(const moltest_reporter *reporter) {
    extra_reporter = reporter;
}

/* ------------------------------------------------------------------ */
/* Recording outcomes                                                   */
/* ------------------------------------------------------------------ */

static void add_failure(const char *check, const char *expected, const char *actual,
                        const char *message, const char *file, int line) {
    current_failed = true;
    if (!ensure_capacity((void **)&failures, failure_count, &failure_capacity,
                         sizeof *failures))
        return;
    failures[failure_count] = (failure_record){
        .test = current_test,
        .check = dup_string(check),
        .expected = dup_string(expected),
        .actual = dup_string(actual),
        .message = dup_string(message),
        .file = file,
        .line = line,
    };
    failure_count++;
}

void moltest_record_failure(const char *message, const char *file, int line) {
    total_assertions++;
    tests[current_test].assertions++;
    add_failure(NULL, NULL, NULL, message, file, line);
}

void moltest_record_warning(const char *message, const char *file, int line) {
    if (!ensure_capacity((void **)&warnings, warning_count, &warning_capacity,
                         sizeof *warnings))
        return;
    warnings[warning_count] = (warning_record){
        .test = current_test,
        .message = dup_string(message),
        .file = file,
        .line = line,
    };
    warning_count++;
    current_warnings++;
}

void moltest_record_skip(const char *reason, const char *file, int line) {
    (void)file;
    (void)line;
    current_skipped = true;
    tests[current_test].skip_reason = reason;
}

/* Count an assertion and, when it failed, record the detail. */
static bool report_check(bool passed, const char *check, const char *expected,
                         const char *actual, const char *file, int line) {
    total_assertions++;
    tests[current_test].assertions++;
    if (!passed)
        add_failure(check, expected, actual, NULL, file, line);
    return passed;
}

/* ------------------------------------------------------------------ */
/* Assertions                                                           */
/* ------------------------------------------------------------------ */

bool moltest_check_bool(bool value, bool expected, const char *expr,
                        const char *file, int line) {
    return report_check(value == expected, expr,
                        expected ? "true" : "false",
                        value ? "true" : "false", file, line);
}

bool moltest_check_int(long long actual, long long expected, bool want_equal,
                       const char *expr, const char *file, int line) {
    bool passed = want_equal ? actual == expected : actual != expected;
    char expected_text[VALUE_BUFFER];
    char actual_text[VALUE_BUFFER];
    snprintf(expected_text, sizeof expected_text, "%s%lld",
             want_equal ? "" : "anything but ", expected);
    snprintf(actual_text, sizeof actual_text, "%lld", actual);
    return report_check(passed, expr, expected_text, actual_text, file, line);
}

bool moltest_check_str(const char *actual, const char *expected, bool want_equal,
                       const char *expr, const char *file, int line) {
    bool equal = actual == expected
              || (actual != NULL && expected != NULL && strcmp(actual, expected) == 0);
    bool passed = want_equal ? equal : !equal;
    char expected_text[VALUE_BUFFER];
    char actual_text[VALUE_BUFFER];
    snprintf(expected_text, sizeof expected_text, "%s\"%s\"",
             want_equal ? "" : "anything but ", expected != NULL ? expected : "(null)");
    snprintf(actual_text, sizeof actual_text, "\"%s\"",
             actual != NULL ? actual : "(null)");
    return report_check(passed, expr, expected_text, actual_text, file, line);
}

bool moltest_check_ptr(const void *actual, const void *expected, bool want_equal,
                       const char *expr, const char *file, int line) {
    bool passed = want_equal ? actual == expected : actual != expected;
    char expected_text[VALUE_BUFFER];
    char actual_text[VALUE_BUFFER];
    snprintf(expected_text, sizeof expected_text, "%s%p",
             want_equal ? "" : "anything but ", expected);
    snprintf(actual_text, sizeof actual_text, "%p", actual);
    return report_check(passed, expr, expected_text, actual_text, file, line);
}

bool moltest_check_null(const void *value, bool want_null, const char *expr,
                        const char *file, int line) {
    bool passed = want_null ? value == NULL : value != NULL;
    char actual_text[VALUE_BUFFER];
    snprintf(actual_text, sizeof actual_text, "%p", value);
    return report_check(passed, expr, want_null ? "NULL" : "non-NULL",
                        value == NULL ? "NULL" : actual_text, file, line);
}

bool moltest_check_order(long long lhs, long long rhs, const char *op,
                         const char *expr, const char *file, int line) {
    bool passed = false;
    if (strcmp(op, "<") == 0)
        passed = lhs < rhs;
    else if (strcmp(op, "<=") == 0)
        passed = lhs <= rhs;
    else if (strcmp(op, ">") == 0)
        passed = lhs > rhs;
    else if (strcmp(op, ">=") == 0)
        passed = lhs >= rhs;
    char expected_text[VALUE_BUFFER];
    char actual_text[VALUE_BUFFER];
    snprintf(expected_text, sizeof expected_text, "a value %s %lld", op, rhs);
    snprintf(actual_text, sizeof actual_text, "%lld", lhs);
    return report_check(passed, expr, expected_text, actual_text, file, line);
}

/* ------------------------------------------------------------------ */
/* Console output                                                       */
/* ------------------------------------------------------------------ */

static struct {
    const char *red, *green, *yellow, *cyan, *dim, *bold, *reset;
} style;

static void set_colors(bool enabled) {
    if (enabled) {
        style.red = "\033[31m";
        style.green = "\033[32m";
        style.yellow = "\033[33m";
        style.cyan = "\033[36m";
        style.dim = "\033[2m";
        style.bold = "\033[1m";
        style.reset = "\033[0m";
    } else {
        style.red = style.green = style.yellow = "";
        style.cyan = style.dim = style.bold = style.reset = "";
    }
}

static void print_banner(const char *text) {
    printf("\n%s%s %s %s%s\n", style.bold, "========================", text,
           "========================", style.reset);
}

static const char *status_mark(moltest_status status) {
    switch (status) {
        case moltest_status_failed:  return "F";
        case moltest_status_skipped: return "s";
        case moltest_status_warned:  return "W";
        case moltest_status_passed:
        default:                     return ".";
    }
}

static const char *status_color(moltest_status status) {
    switch (status) {
        case moltest_status_failed:  return style.red;
        case moltest_status_skipped: return style.yellow;
        case moltest_status_warned:  return style.yellow;
        case moltest_status_passed:
        default:                     return style.green;
    }
}

static const char *status_label(moltest_status status) {
    switch (status) {
        case moltest_status_failed:  return "FAILED";
        case moltest_status_skipped: return "SKIPPED";
        case moltest_status_warned:  return "WARNING";
        case moltest_status_passed:
        default:                     return "PASSED";
    }
}

/* Print `text` indented, so captured output is easy to tell apart. */
static void print_indented(const char *text) {
    printf("    ");
    for (const char *p = text; *p != '\0'; p++) {
        putchar(*p);
        if (*p == '\n' && p[1] != '\0')
            printf("    ");
    }
    if (text[0] != '\0' && text[strlen(text) - 1] != '\n')
        putchar('\n');
}

static void print_failures(void) {
    if (failure_count == 0)
        return;
    print_banner("FAILURES");
    /* One section per failing test, listing all of its failed assertions. */
    for (size_t ti = 0; ti < test_count; ti++) {
        if (tests[ti].status != moltest_status_failed)
            continue;
        printf("%s%s::%s%s\n", style.bold, tests[ti].file, tests[ti].name, style.reset);
        for (size_t i = 0; i < failure_count; i++) {
            const failure_record *f = &failures[i];
            if (f->test != ti)
                continue;
            if (f->message != NULL)
                printf("  %sFAIL%s: %s\n", style.red, style.reset, f->message);
            else
                printf("  %s%s%s failed\n", style.red, f->check, style.reset);
            if (f->expected != NULL)
                printf("    Expected: %s\n", f->expected);
            if (f->actual != NULL)
                printf("      Actual: %s\n", f->actual);
            printf("    Location: %s%s:%d%s\n", style.dim, f->file, f->line, style.reset);
        }
        if (tests[ti].output != NULL && tests[ti].output[0] != '\0') {
            printf("  %scaptured output:%s\n", style.dim, style.reset);
            print_indented(tests[ti].output);
        }
        printf("\n");
    }
}

static void print_warnings(void) {
    if (warning_count == 0)
        return;
    print_banner("WARNINGS");
    for (size_t i = 0; i < warning_count; i++) {
        const warning_record *w = &warnings[i];
        printf("  %s%s:%d%s: %s%s%s\n", style.dim, w->file, w->line, style.reset,
               style.yellow, w->message, style.reset);
    }
    printf("\n");
}

static void print_summary(const moltest_summary *summary) {
    const char *color = summary->failed > 0 ? style.red
                      : (summary->warned > 0 || summary->skipped > 0) ? style.yellow
                      : style.green;
    printf("%s%s%zu passed", color, style.bold, summary->passed);
    if (summary->failed > 0)
        printf(", %zu failed", summary->failed);
    if (summary->skipped > 0)
        printf(", %zu skipped", summary->skipped);
    if (summary->warnings > 0)
        printf(", %zu warning%s", summary->warnings, summary->warnings == 1 ? "" : "s");
    printf(" — %zu assertions in %.2fs%s\n", summary->assertions, summary->seconds,
           style.reset);
}

/* ------------------------------------------------------------------ */
/* Runner                                                               */
/* ------------------------------------------------------------------ */

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- output capture: a test's own printing is hidden unless it fails ---- */

static bool capture_enabled = true;
static FILE *capture_file;
static int saved_stdout = -1;
static int saved_stderr = -1;

/* Redirect the real file descriptors, so output from child processes started by
   the test is captured too. */
static void capture_start(void) {
    if (!capture_enabled)
        return;
    fflush(stdout);
    fflush(stderr);
    capture_file = tmpfile();
    if (capture_file == NULL)
        return;
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    dup2(fileno(capture_file), STDOUT_FILENO);
    dup2(fileno(capture_file), STDERR_FILENO);
}

/* Restore the descriptors and return what was captured (heap, may be NULL). */
static char *capture_finish(void) {
    if (capture_file == NULL)
        return NULL;
    fflush(stdout);
    fflush(stderr);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);

    char *text = NULL;
    if (fseek(capture_file, 0, SEEK_END) == 0) {
        long size = ftell(capture_file);
        if (size > 0) {
            size_t wanted = (size_t)size < CAPTURE_LIMIT ? (size_t)size : CAPTURE_LIMIT;
            rewind(capture_file);
            text = malloc(wanted + 1);
            if (text != NULL) {
                size_t read = fread(text, 1, wanted, capture_file);
                text[read] = '\0';
            }
        }
    }
    fclose(capture_file);
    capture_file = NULL;
    return text;
}

/* Distinct files holding selected tests, in registration order; `out_files`
   must have room for test_count entries. */
static size_t collect_files(const char **out_files) {
    size_t count = 0;
    for (size_t i = 0; i < test_count; i++) {
        if (!tests[i].selected)
            continue;
        bool seen = false;
        for (size_t j = 0; j < count && !seen; j++)
            seen = strcmp(out_files[j], tests[i].file) == 0;
        if (!seen)
            out_files[count++] = tests[i].file;
    }
    return count;
}

static void run_one(size_t index) {
    test_entry *t = &tests[index];
    current_test = index;
    current_failed = false;
    current_skipped = false;
    current_warnings = 0;

    if (t->skip_reason != NULL) {
        t->status = moltest_status_skipped;
        return;
    }

    double started = now_seconds();
    capture_start();
    t->fn();
    t->output = capture_finish();
    t->seconds = now_seconds() - started;

    if (current_failed)
        t->status = moltest_status_failed;
    else if (current_skipped)
        t->status = moltest_status_skipped;
    else if (current_warnings > 0)
        t->status = moltest_status_warned;
    else
        t->status = moltest_status_passed;
}

static void print_usage(void) {
    printf("moltest %s\n\n", MOLTEST_VERSION);
    printf("Usage:\n    <test-binary> [options]\n\n");
    printf("Options:\n");
    printf("    -k <substring>   Only run tests whose file or name matches\n");
    printf("    -v               One line per test\n");
    printf("    -s               Do not capture what the tests print\n");
    printf("    --list           List the registered tests and exit\n");
    printf("    --color=<when>   auto (default), always or never\n");
    printf("    -h, --help       Show this help\n");
}

int moltest_run(int argc, char **argv) {
    const char *filter = NULL;
    bool verbose = false;
    bool list_only = false;
    int color_mode = 0; /* 0 auto, 1 always, 2 never */

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-k") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (strcmp(arg, "-v") == 0) {
            verbose = true;
        } else if (strcmp(arg, "-s") == 0) {
            capture_enabled = false;
        } else if (strcmp(arg, "--list") == 0) {
            list_only = true;
        } else if (strncmp(arg, "--color=", 8) == 0) {
            const char *when = arg + 8;
            color_mode = strcmp(when, "always") == 0 ? 1
                       : strcmp(when, "never") == 0 ? 2 : 0;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        } else {
            fprintf(stderr, "moltest: unknown option '%s'\n", arg);
            print_usage();
            return 1;
        }
    }

    bool color = color_mode == 1
              || (color_mode == 0 && isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL);
    set_colors(color);

    /* Apply the filter. */
    size_t selected = 0;
    for (size_t i = 0; i < test_count; i++) {
        tests[i].selected = filter == NULL
                         || strstr(tests[i].name, filter) != NULL
                         || strstr(tests[i].file, filter) != NULL;
        if (tests[i].selected)
            selected++;
    }

    if (list_only) {
        for (size_t i = 0; i < test_count; i++) {
            if (tests[i].selected)
                printf("%s::%s\n", tests[i].file, tests[i].name);
        }
        return 0;
    }

    const char **files = calloc(test_count > 0 ? test_count : 1, sizeof *files);
    if (files == NULL)
        return 1;
    size_t file_count = collect_files(files);

    printf("%smoltest %s%s — %zu file%s, %zu test%s\n\n", style.bold, MOLTEST_VERSION,
           style.reset, file_count, file_count == 1 ? "" : "s",
           selected, selected == 1 ? "" : "s");
    if (extra_reporter != NULL && extra_reporter->on_run_start != NULL)
        extra_reporter->on_run_start(file_count, selected, extra_reporter->ctx);

    double started = now_seconds();
    size_t done = 0;
    moltest_summary summary = { .files = file_count, .tests = selected };

    for (size_t f = 0; f < file_count; f++) {
        const char *file = files[f];
        size_t in_file = 0;
        for (size_t i = 0; i < test_count; i++)
            in_file += (tests[i].selected && strcmp(tests[i].file, file) == 0) ? 1 : 0;
        if (in_file == 0)
            continue;

        if (extra_reporter != NULL && extra_reporter->on_file_start != NULL)
            extra_reporter->on_file_start(file, extra_reporter->ctx);

        int written = 0;
        if (!verbose)
            written = printf("%s ", file);

        for (size_t i = 0; i < test_count; i++) {
            if (!tests[i].selected || strcmp(tests[i].file, file) != 0)
                continue;
            run_one(i);
            done++;
            switch (tests[i].status) {
                case moltest_status_failed:  summary.failed++; break;
                case moltest_status_skipped: summary.skipped++; break;
                case moltest_status_warned:  summary.warned++; summary.passed++; break;
                case moltest_status_passed:
                default:                     summary.passed++; break;
            }
            if (verbose) {
                printf("%s%-7s%s %s::%s %s(%.3fs)%s\n", status_color(tests[i].status),
                       status_label(tests[i].status), style.reset, file, tests[i].name,
                       style.dim, tests[i].seconds, style.reset);
            } else {
                written += printf("%s%s%s", status_color(tests[i].status),
                                  status_mark(tests[i].status), style.reset);
            }
            if (extra_reporter != NULL && extra_reporter->on_test_end != NULL)
                extra_reporter->on_test_end(file, tests[i].name, tests[i].status,
                                            tests[i].seconds, extra_reporter->ctx);
        }

        if (!verbose) {
            /* Colour escapes do not occupy columns, so track the visible width. */
            int visible = (int)strlen(file) + 1 + (int)in_file;
            for (int pad = visible; pad < PROGRESS_COLUMN; pad++)
                putchar(' ');
            printf("%s[%3zu%%]%s\n", style.dim, selected == 0 ? 100 : done * 100 / selected,
                   style.reset);
            (void)written;
        }
        if (extra_reporter != NULL && extra_reporter->on_file_end != NULL)
            extra_reporter->on_file_end(file, done, selected, extra_reporter->ctx);
    }

    summary.seconds = now_seconds() - started;
    summary.assertions = total_assertions;
    summary.warnings = warning_count;

    print_failures();
    print_warnings();
    if (failure_count == 0 && warning_count == 0)
        printf("\n");
    print_summary(&summary);

    if (extra_reporter != NULL && extra_reporter->on_run_end != NULL)
        extra_reporter->on_run_end(&summary, extra_reporter->ctx);

    /* Release the recorded details. */
    for (size_t i = 0; i < failure_count; i++) {
        free(failures[i].check);
        free(failures[i].expected);
        free(failures[i].actual);
        free(failures[i].message);
    }
    for (size_t i = 0; i < warning_count; i++)
        free(warnings[i].message);
    for (size_t i = 0; i < test_count; i++)
        free(tests[i].output);
    free(failures);
    free(warnings);
    free(files);
    free(tests);

    return summary.failed > 0 ? 1 : 0;
}
