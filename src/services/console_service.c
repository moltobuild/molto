#include <molto/services/console_service.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

/*
 * The platform, in one block. Both systems do the same three things — ask
 * whether stdin is a terminal, take its current mode, and put it back — and
 * they spell every one of them differently.
 */

#ifdef _WIN32

typedef DWORD console_mode;

static bool stdin_is_a_terminal(void) { return _isatty(_fileno(stdin)) != 0; }

static bool mode_take(console_mode *out) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    return input != INVALID_HANDLE_VALUE && GetConsoleMode(input, out) != 0;
}

static bool mode_hide_echo(console_mode original) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    return SetConsoleMode(input, original & ~(DWORD)ENABLE_ECHO_INPUT) != 0;
}

static void mode_restore(console_mode original) {
    (void)SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), original);
}

#else

typedef struct termios console_mode;

static bool stdin_is_a_terminal(void) { return isatty(STDIN_FILENO) != 0; }

static bool mode_take(console_mode *out) { return tcgetattr(STDIN_FILENO, out) == 0; }

static bool mode_hide_echo(console_mode original) {
    console_mode hidden = original;
    hidden.c_lflag &= (tcflag_t)~ECHO;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) == 0;
}

static void mode_restore(console_mode original) {
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
}

#endif

/* Reads a line, dropping the newline. False at end of input. */
static bool read_line(char *out, size_t size) {
    if(fgets(out, (int)size, stdin) == NULL)
        return false;
    out[strcspn(out, "\r\n")] = '\0';
    return true;
}

console_secret console_read_secret(const char *prompt, char *out, size_t size) {
    if(!stdin_is_a_terminal())
        return console_secret_not_a_terminal;

    console_mode original;
    if(!mode_take(&original))
        return console_secret_no_control;
    if(!mode_hide_echo(original))
        return console_secret_no_control;

    fputs(prompt, stderr);
    fflush(stderr);
    const bool typed = read_line(out, size) && out[0] != '\0';

    /* Unconditional: the terminal goes back the way it was found whatever the
       read did, including nothing. */
    mode_restore(original);
    fputs("\n", stderr);

    return typed ? console_secret_ok : console_secret_empty;
}
