#ifndef MOLTO_CONSOLE_SERVICE_H
#define MOLTO_CONSOLE_SERVICE_H

#include <stddef.h>

/* How reading a secret went.

   Four outcomes rather than a bool, because the three failures want three
   different things said to the person, and choosing the words is the command's
   job rather than this one's (RFC-0017 keeps the platform in the services;
   RFC-0011 keeps the wording where the user is). */
typedef enum {
    console_secret_ok,
    console_secret_not_a_terminal, /* stdin is a pipe: a script wrote it down */
    console_secret_no_control,     /* the terminal would not turn echo off */
    console_secret_empty,          /* end of input, or nothing typed */
} console_secret;

/* Print `prompt` on stderr, read a line from stdin without echoing it, and
   restore the terminal however it goes.

   Echo is off for exactly the span of the read, and it is turned back on even
   when the read fails — a terminal left silent after a command exits is worse
   than the password being visible, because the person cannot see what they
   type next either. */
[[nodiscard]] console_secret console_read_secret(const char *prompt, char *out, size_t size);

#endif /* MOLTO_CONSOLE_SERVICE_H */
