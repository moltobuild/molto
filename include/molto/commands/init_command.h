#ifndef MOLTO_INIT_COMMAND_H
#define MOLTO_INIT_COMMAND_H

#include <molto/services/manifest_service.h>

/* Execute `molto init`: scaffold a project in the current directory, a
   library unless `kind` says binary. Returns a molto_exit_code. */
[[nodiscard]] int init_command_run(project_kind kind);

#endif /* MOLTO_INIT_COMMAND_H */
