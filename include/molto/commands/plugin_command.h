#ifndef MOLTO_PLUGIN_COMMAND_H
#define MOLTO_PLUGIN_COMMAND_H

#include <stdbool.h>
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

   `install` fetches from a registry and asks before it does: what a plugin may
   do is the thing being consented to, so the permissions are printed and the
   question is asked, unless `assume_yes` says a caller already decided.
   `remove` uninstalls what molto installed, and refuses to touch a plugin that
   arrived on PATH some other way.

   Returns a molto_exit_code. */
[[nodiscard]] int plugin_command_run(const char *action, const char *name, const char *registry,
                                     bool assume_yes);

#endif /* MOLTO_PLUGIN_COMMAND_H */
