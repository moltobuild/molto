#ifndef MOLTO_CONSOLE_SERVICE_H
#define MOLTO_CONSOLE_SERVICE_H

#include <stdbool.h>
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

/* One stream's console mode, kept so it can be put back. `changed` false is
   both "there was nothing to change" and "the change did not take", which want
   the same thing done about them: nothing. */
typedef struct {
    unsigned long mode;
    bool changed;
} console_stream_mode;

/* The console's output settings, as they were found, so they can be put back.

   Carried from `console_output_prepare` to `console_output_restore` and read by
   nobody else: what is in it is one platform's idea of a console, and the
   other's is nothing at all. */
typedef struct {
    unsigned int code_page; /* what to go back to; 0 when nothing was changed */
    console_stream_mode out;
    console_stream_mode err; /* both, because the build report prints to stderr */
} console_output;

/* Make the console able to show what molto prints, and hand back what it was.

   Molto's output is UTF-8 — the inventory's `○`, the `✓` on the line that
   finishes a build — but a Windows console decodes bytes with whatever code
   page it was started in, and the machines this matters on are not started in
   65001: a console in 850 draws each of those three bytes as a letter, so
   `✓ Finished` arrives as `Ô£ô Finished`. The same call turns on virtual
   terminal processing, without which the colours are escapes printed literally.

   POSIX has neither knob and needs neither: a terminal there is handed bytes
   and reads them as the locale says, which molto does not get a say in and
   should not take one. There the call changes nothing, returns a token that
   restores nothing, and the output is exactly what it has always been.

   Both changes belong to the console rather than to the process, so they
   outlive molto and are visible to whatever runs next in the same window.
   That is what `console_output_restore` is for, and why the pair is taken at
   the top of `main` rather than around each thing that prints. */
[[nodiscard]] console_output console_output_prepare(void);

/* Put back what `console_output_prepare` changed. Safe on a token that changed
   nothing, which is the only kind POSIX produces. */
void console_output_restore(console_output original);

#endif /* MOLTO_CONSOLE_SERVICE_H */
