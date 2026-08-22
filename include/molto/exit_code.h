#ifndef MOLTO_EXIT_CODE_H
#define MOLTO_EXIT_CODE_H

/* Process exit codes as specified in RFC-0002. */
typedef enum {
    exit_ok = 0,
    exit_build_failure = 1,
    exit_invalid_manifest = 2,
    exit_dependency_failure = 3,
    exit_usage_error = 4,
    /* The command exists in the CLI but has no implementation yet. Distinct
       from a build failure so a script can tell "this broke" from "this does
       not exist yet". */
    exit_not_implemented = 5,
    /* A plugin broke: it could not be run, it timed out, or it answered with
       something that is not valid (RFC-0014). Distinct from a build failure so
       a script can tell "my code does not compile" from "a third-party binary
       misbehaved". */
    exit_plugin_failure = 6,
} molto_exit_code;

#endif /* MOLTO_EXIT_CODE_H */
