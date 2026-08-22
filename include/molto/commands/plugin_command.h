#ifndef MOLTO_PLUGIN_COMMAND_H
#define MOLTO_PLUGIN_COMMAND_H

#include <stddef.h>

/* Execute `molto plugin <list|info> [<name>]` (RFC-0014).

   `list` reports every plugin this machine offers: its name, where it came
   from, and the capabilities and permissions its recipe declares. `info`
   reports one in full.

   Both show permissions rather than hiding them behind a flag. The security of
   the whole design rests on a person being able to answer "what is installed,
   and what may it do" without reading a recipe by hand, and a listing that
   omits the answer moves that question somewhere nobody looks.

   A plugin found on PATH usually has no recipe beside it — nothing installed
   it, so nothing recorded what it asked for. That is reported as the unknown it
   is, never as "asks for nothing".

   Returns a molto_exit_code. */
[[nodiscard]] int plugin_command_run(const char *action, const char *name);

#endif /* MOLTO_PLUGIN_COMMAND_H */
