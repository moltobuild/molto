#ifndef MOLTO_NEW_COMMAND_H
#define MOLTO_NEW_COMMAND_H

#include <molto/services/manifest_service.h>

/* Execute `molto new <name>`: scaffold a new project directory, a library
   unless `kind` says binary. Returns a molto_exit_code. */
[[nodiscard]] int new_command_run(const char *name, project_kind kind);

#endif /* MOLTO_NEW_COMMAND_H */
