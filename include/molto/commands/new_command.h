#ifndef MOLTO_NEW_COMMAND_H
#define MOLTO_NEW_COMMAND_H

/* Execute `molto new <name>`: scaffold a new project directory.
   Returns a molto_exit_code. */
[[nodiscard]] int new_command_run(const char *name);

#endif /* MOLTO_NEW_COMMAND_H */
