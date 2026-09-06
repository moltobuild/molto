#ifndef MOLTO_PATHS_SERVICE_H
#define MOLTO_PATHS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Where molto keeps what belongs to the person rather than to a project: the
 * registry credential, installed plugins, the cache a source is unpacked into.
 *
 * One answer in one place, because it was being worked out in four and every
 * copy read `HOME` and nothing else. That is the whole environment on a Unix
 * and it is not on Windows, where cmd and PowerShell set `USERPROFILE` and
 * leave `HOME` unset — only a shell that brings its own environment, MSYS2 or
 * git-bash, defines it. So `molto login` answered "HOME is not set, so there is
 * nowhere to store credentials" on a machine with a perfectly ordinary home
 * directory, and the way to log in was to find a different shell.
 */

/* Room for a composed path under the molto home. */
#define MOLTO_HOME_PATH_MAX 512

/* The directory molto keeps its own things in: `$MOLTO_HOME` where that is set,
   and `<home>/.molto` otherwise. False when there is no home to derive one
   from, which is the one case a caller has to report rather than work around.

   The override is the same escape hatch `$MOLTO_CACHE` already gives the cache
   and `$PICKUP_HOME` gives pickup: somewhere to point a machine whose home is
   not writable, and what a test sets so it never touches the real one. */
[[nodiscard]] bool paths_molto_home(char *out, size_t size);

/* Compose `subdirectory` under it — "cache", "plugins/bin". */
[[nodiscard]] bool paths_molto_subdir(const char *subdirectory, char *out, size_t size);

#endif /* MOLTO_PATHS_SERVICE_H */
