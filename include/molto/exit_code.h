#ifndef MOLTO_EXIT_CODE_H
#define MOLTO_EXIT_CODE_H

/* Process exit codes as specified in RFC-0002. */
typedef enum {
    exit_ok = 0,
    exit_build_failure = 1,
    exit_invalid_manifest = 2,
    exit_dependency_failure = 3,
    exit_usage_error = 4,
} molto_exit_code;

#endif /* MOLTO_EXIT_CODE_H */
