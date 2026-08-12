#ifndef MOLTO_PUBLISH_COMMAND_H
#define MOLTO_PUBLISH_COMMAND_H

#include <stdbool.h>

/* Execute `molto publish [--recipe <path>] [--file <archive>] [--dry-run]`.

   Publishes one artifact: the recipe describes the coordinate, the archive is
   its bytes. Both default to what is in the current directory — `recipe.toml`
   and the single `.tar.zst` beside it.

   The recipe is validated and the archive hashed before anything is sent, so a
   mistake costs nothing. Then the bytes go up and the row is written, in that
   order: a catalogue entry without its blob is the one inconsistency the
   registry cannot serve around.

   A recipe declaring `form = "source"` has no bytes at all (RFC-0009): it names
   an upstream archive or commit and a build system, and the consumer fetches
   and builds it. Nothing is looked for, hashed or uploaded for one, and naming
   an archive with `--file` is an error rather than an argument to ignore.

   `recipe` and `file` may be NULL to use the defaults. Returns a
   molto_exit_code. */
[[nodiscard]] int publish_command_run(const char *recipe, const char *file, bool dry_run);

#endif /* MOLTO_PUBLISH_COMMAND_H */
