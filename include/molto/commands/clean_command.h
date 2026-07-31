#ifndef MOLTO_CLEAN_COMMAND_H
#define MOLTO_CLEAN_COMMAND_H

#include <stdbool.h>

/* Execute `molto clean [--all]`: delete the workspace's `build/` directory.
   With `all`, also delete `.bin/`, discarding the incremental state so the
   next build starts from nothing. Returns a molto_exit_code. */
[[nodiscard]] int clean_command_run(bool all);

#endif /* MOLTO_CLEAN_COMMAND_H */
