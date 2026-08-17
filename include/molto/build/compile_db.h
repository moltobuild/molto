#ifndef MOLTO_COMPILE_DB_H
#define MOLTO_COMPILE_DB_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/util/str_list.h>

/*
 * The compilation database: `compile_commands.json` at the project root.
 *
 * Every tool that has to parse C or C++ without being the build has the same
 * problem — it cannot resolve an `#include` without knowing the search path,
 * and cannot read an `#ifdef` without knowing the defines. clangd (and behind
 * it VS Code, neovim, Emacs, Helix, Zed), clang-tidy, cppcheck and
 * include-what-you-use all answer it the same way: they walk up from the file
 * they are looking at until they find this file. Without it they guess, and
 * they guess wrong.
 *
 * Molto is in a position to answer exactly, because it already composes the
 * command line for every translation unit in order to fingerprint it. This
 * collects those command lines as they are composed and writes them out once.
 *
 * What lands in `arguments` is what is executed, verbatim. A database that
 * prettified its own paths would be describing a compilation that never
 * happened, and the difference would surface as a tool disagreeing with the
 * build about what compiles.
 */
typedef struct compile_db compile_db;

/* An empty database. NULL if memory ran out. Free it with compile_db_destroy. */
[[nodiscard]] compile_db *compile_db_create(void);

/* Release the database and every entry in it. Safe on NULL. */
void compile_db_destroy(compile_db *db);

/* Record one translation unit: the source, the object it compiles to, and the
   full argv (driver included). All three are copied, so the caller may free
   them straight afterwards. Adding to a NULL database succeeds and does
   nothing, which is what lets a caller that wants no database pass NULL
   instead of every call site testing for it. */
[[nodiscard]] bool compile_db_add(compile_db *db, const char *file, const char *output,
                                  const str_list *arguments);

/* How many units have been recorded. Zero for NULL. */
[[nodiscard]] size_t compile_db_count(const compile_db *db);

/* Write `root/compile_commands.json`, in the order the units were added.
   Written to a temporary file and renamed into place, so a tool reading it
   concurrently sees either the previous database or this one, never half of
   one. Returns false if it could not be written; the build is not affected by
   that, only the editor's view of it. */
[[nodiscard]] bool compile_db_write(const compile_db *db, const char *root);

#endif /* MOLTO_COMPILE_DB_H */
