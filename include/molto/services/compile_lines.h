#ifndef MOLTO_COMPILE_LINES_H
#define MOLTO_COMPILE_LINES_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/util/str_list.h>

/*
 * What upstream's build would compile, read from the build itself (RFC-0025).
 *
 * A delegated recipe with `[build].sources` asks the configured build instead
 * of listing its files: `make -n` prints every compile line make would run,
 * and a CMake configuration writes compile_commands.json. Either way the
 * answer is a list of lines, and each line is read the same way: the file it
 * compiles, inside the source, and the arguments upstream gives it — minus the
 * driver, the input, `-c`, `-o` and the dependency-file flags, which are
 * molto's to choose. A relative include is made absolute against the
 * directory the line ran in, because molto compiles from elsewhere.
 *
 * Anything that is not a compile line by one of the drivers named — `ar`,
 * `ranlib`, a script writing a version header — is ignored: molto archives and
 * links on its own.
 */

typedef struct {
    char *source;  /* relative to the source root, forward slashes */
    str_list args; /* upstream's arguments, in order */
} compile_line;

typedef struct {
    compile_line *lines;
    size_t count;
    size_t capacity;
} compile_lines;

void compile_lines_init(compile_lines *out);
void compile_lines_free(compile_lines *out);

/* The programs whose lines are compile lines: the C and C++ compilers molto
   passed and, when one was resolved, NASM. A line matches by the exact path,
   slashes either way. Any may be NULL. */
typedef struct {
    const char *cc;
    const char *cxx;
    const char *nasm;
} compile_drivers;

/* Every compile line in what `make -n` printed, with `root` the directory make
   ran in. `make[1]: Entering directory` lines move where a recursive make's
   relative paths are anchored. False with a message for a line naming a file
   outside `root`. */
[[nodiscard]] bool compile_lines_from_make(const char *output, const char *root,
                                           const compile_drivers *drivers, compile_lines *out,
                                           char *err, size_t err_size);

/* Every entry of a compile_commands.json, `command` or `arguments`, each
   anchored at its own `directory`. */
[[nodiscard]] bool compile_lines_from_database(const char *json, const char *root,
                                               const compile_drivers *drivers, compile_lines *out,
                                               char *err, size_t err_size);

/* Kept beside the configuration, so a build whose stamp holds reads the list
   instead of asking make again. The format is compile_commands.json's own,
   `file` relative to the root and `arguments` without a driver. */
[[nodiscard]] bool compile_lines_write(const compile_lines *lines, const char *path);
[[nodiscard]] bool compile_lines_read(const char *path, compile_lines *out);

/* `text` split into words as `sh` splits a simple command: blanks separate,
   single quotes are literal, double quotes and backslashes escape. Exposed
   for its tests. */
[[nodiscard]] bool compile_lines_split(const char *text, str_list *out);

#endif /* MOLTO_COMPILE_LINES_H */
