#ifndef MOLTO_INIT_COMMAND_H
#define MOLTO_INIT_COMMAND_H

/* Execute `molto init`: scaffold a project in the current directory.
   Returns a molto_exit_code. */
[[nodiscard]] int init_command_run(void);

#endif /* MOLTO_INIT_COMMAND_H */
