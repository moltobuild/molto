#ifndef MOLTO_FMT_SERVICE_H
#define MOLTO_FMT_SERVICE_H

#include <stdbool.h>
#include <stdio.h>

#include <molto/build/diagnostic.h>
#include <molto/util/str_list.h>

/* What `molto fmt` does with what the formatter produces. */
typedef enum {
    fmt_mode_write, /* rewrite the file in place: the default */
    fmt_mode_check, /* report what would change and write nothing: for CI */
    fmt_mode_diff,  /* print the unified diff and write nothing */
} fmt_mode;

typedef struct {
    fmt_mode mode;
    bool refresh_tools;
    /* Format every file again instead of skipping the ones already recorded as
       formatted (RFC-0006). */
    bool refresh_analysis;
    /* Where fmt_mode_diff writes its diffs. A stream rather than a buffer
       because a diff is unbounded; the service still decides nothing about
       presentation, only what to put in it. NULL in the other modes. */
    FILE *diff_stream;
} fmt_request;

typedef struct {
    diagnostic_list diagnostics; /* what the formatter said, normalized */
    str_list changed;            /* paths that changed, or would have */
} fmt_result;

void fmt_result_init(fmt_result *result);
void fmt_result_free(fmt_result *result);

/*
 * Format the project rooted at `root`, or report what formatting would do.
 *
 * Sources come from `src/` and `include/`, minus what format.json excludes, and
 * headers are included: RFC-0005 is explicit that style applies to a `.h` as
 * much as to a `.c`. The backend is whatever `pickup tools` reports, run
 * against a configuration Molto generates under `.bin/` from format.json, so
 * the project tree never grows a .clang-format of its own.
 *
 * Nothing is printed here. Returns a molto_exit_code describing the run and not
 * its findings:
 *   0  the formatter ran (what it found is in `result`)
 *   1  the formatter could not be run, or a file could not be read
 *   2  format.json is invalid, or asks for something that cannot be translated
 *   3  this machine has no formatter
 */
[[nodiscard]] int fmt_project(const char *root, const fmt_request *request, fmt_result *result);

#endif /* MOLTO_FMT_SERVICE_H */
