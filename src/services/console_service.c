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

/*
 * The output side of the same divide.
 *
 * On Windows the console decides for itself how to read the bytes a program
 * hands it, and the answer it was started with is rarely the one molto writes.
 * On POSIX there is nothing here to decide: the bytes go out as they are and
 * the terminal reads them by the locale, so both functions do nothing at all
 * and the output on those systems is byte for byte what it was.
 */

#ifdef _WIN32

/* Turn escape processing on for one standard stream, reporting what has to be
   undone. Nothing is reported — and nothing is undone later — when the stream
   is redirected, when the console already had the bit, or when the call is
   refused: the mode molto found is the mode it leaves. */
static console_stream_mode vt_enable(DWORD stream) {
    const console_stream_mode unchanged = {0};
    const HANDLE handle = GetStdHandle(stream);
    DWORD mode = 0;
    if(handle == NULL || handle == INVALID_HANDLE_VALUE || GetConsoleMode(handle, &mode) == 0)
        return unchanged;
    if((mode & (DWORD)ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
        return unchanged;
    if(SetConsoleMode(handle, mode | (DWORD)ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0)
        return unchanged;
    return (console_stream_mode){.mode = mode, .changed = true};
}

static void vt_restore(DWORD stream, console_stream_mode saved) {
    if(saved.changed)
        (void)SetConsoleMode(GetStdHandle(stream), (DWORD)saved.mode);
}

console_output console_output_prepare(void) {
    console_output found = {0};

    /* The code page belongs to the console rather than to a stream, so asking
       for it is also how molto asks whether there is a console at all: zero
       means nothing is attached, the output is going to a pipe or a file that
       takes the UTF-8 unaltered, and there is nothing here to set. */
    const UINT code_page = GetConsoleOutputCP();
    if(code_page != 0 && code_page != CP_UTF8 && SetConsoleOutputCP(CP_UTF8) != 0)
        found.code_page = code_page;

    found.out = vt_enable(STD_OUTPUT_HANDLE);
    found.err = vt_enable(STD_ERROR_HANDLE);

    return found;
}

void console_output_restore(console_output original) {
    vt_restore(STD_ERROR_HANDLE, original.err);
    vt_restore(STD_OUTPUT_HANDLE, original.out);
    if(original.code_page != 0)
        (void)SetConsoleOutputCP(original.code_page);
}

#else

console_output console_output_prepare(void) { return (console_output){0}; }

void console_output_restore(console_output original) { (void)original; }

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
