#ifndef MOLTO_IR_COMMAND_H
#define MOLTO_IR_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/* Execute `molto ir` (RFC-0013): write the build intermediate representation for
   the project in this directory to standard output, or to `output_path`.
 *
 * It runs the frontend and stops before the engine. There is nothing else to
 * run yet — transforms are RFC-0015's and do not exist — so what it prints is
 * the frontend's answer, validated.
 *
 * It exists because a contract that cannot be inspected is a contract nobody can
 * conform to. A frontend plugin is tested by comparing the document it produces
 * against a fixture, and without this command there is no way to see that
 * document at all: the author of a Meson frontend would be writing against a
 * specification with no way to check their work.
 *
 * `profile` decides which profile's options are folded in, because the profile
 * decides which defines are in force and a `#ifdef` decides what compiles. A
 * document dumped for the wrong profile describes code the build never sees.
 *
 * Two runs over one project produce one byte-identical file — `molto metadata`'s
 * rule, for the reason that command gives: a dump that differs between runs
 * cannot be diffed, and a document that cannot be diffed cannot be reviewed.
 *
 * Returns a molto_exit_code: exit_plugin_failure when a plugin frontend broke or
 * returned something invalid, exit_invalid_manifest when nothing here describes
 * a project. */
[[nodiscard]] int ir_command_run(const char *output_path, const char *profile);

#endif /* MOLTO_IR_COMMAND_H */
