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
 *
 * The three kinds of thing go where the platform keeps that kind of thing,
 * the same way pickup does:
 *
 *   - config, the credential: `$XDG_CONFIG_HOME/molto`, or `~/.config/molto`;
 *   - data, installed plugins: `$XDG_DATA_HOME/molto`, or `~/.local/share/molto`;
 *   - cache, sources and objects: `$XDG_CACHE_HOME/molto`, or `~/.cache/molto`.
 *
 * On Linux and on macOS alike, because a developer's dotfiles, backups and
 * cleaners already know those three. An XDG variable that is empty or not an
 * absolute path is ignored, as the specification asks. On Windows all three
 * live under `%APPDATA%\molto`, with the cache in `cache\` beneath it.
 *
 * `$MOLTO_HOME`, where it is set, overrides all of it with the single directory
 * molto used to keep in `~/.molto`: the credential at its root, `plugins/`
 * and `cache/` inside it. `$MOLTO_CACHE` still moves the cache on its own.
 */

/* Room for a composed path under a molto directory. */
#define MOLTO_HOME_PATH_MAX 512

/* The directory each kind of thing is kept in. False when there is nothing to
   derive one from — no home, no `%APPDATA%` — which is the one case a caller
   has to report rather than work around.

   The override is the same escape hatch `$MOLTO_CACHE` already gives the cache
   and `$PICKUP_HOME` gives pickup: somewhere to point a machine whose home is
   not writable, and what a test sets so it never touches the real one. */
[[nodiscard]] bool paths_molto_config_dir(char *out, size_t size);
[[nodiscard]] bool paths_molto_data_dir(char *out, size_t size);
[[nodiscard]] bool paths_molto_cache_dir(char *out, size_t size);

/* Compose `subdirectory` under the data directory — "plugins/bin". */
[[nodiscard]] bool paths_molto_data_subdir(const char *subdirectory, char *out, size_t size);

/* Where molto kept everything before it followed the platform: `<home>/.molto`,
   whatever `$MOLTO_HOME` says. Only the migration reads it. */
[[nodiscard]] bool paths_molto_legacy_home(char *out, size_t size);

/* Move what an older molto left in `~/.molto` to where it is kept now.

   Done once, by the program rather than by every caller, and a no-op when
   `$MOLTO_HOME` is set (that directory is still the one in use) or when there
   is no `~/.molto`. The credential is renamed, so it is never a copy another
   account could read, and kept at mode 0600; plugins are moved; the cache is
   discarded, because it refills on demand and its objects are keyed by paths
   that pointed into the old place. Nothing that already exists at the new place
   is overwritten, and `~/.molto` is removed only once it is empty.

   True when something was moved, with one or two lines in `note` saying what;
   false with `note` empty when there was nothing to do. */
bool paths_migrate_legacy_home(char *note, size_t note_size);

#endif /* MOLTO_PATHS_SERVICE_H */
