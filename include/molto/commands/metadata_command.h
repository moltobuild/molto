#ifndef MOLTO_METADATA_COMMAND_H
#define MOLTO_METADATA_COMMAND_H

#include <stdbool.h>

/* Execute `molto metadata [--output <path>] [--include-dev]`.

   Writes a CycloneDX 1.6 bill of materials describing the package and
   everything it links: the exact version, origin, checksum and licence of each,
   and the edges between them.

   `output_path` is NULL for stdout, which is where a document meant to be piped
   belongs; progress and warnings go to stderr either way, so
   `molto metadata > sbom.json` leaves a file a reader will take.

   `include_dev` adds the packages reachable only through `[dev-deps]`, marked
   as not shipping. They are out by default: a bill of materials describes what
   is in the artifact, and a test framework is not.

   Resolving the graph is what this costs. In a project that has been built it
   touches no network — a published coordinate is immutable, so the registry's
   previous answer still stands — and in a fresh clone it fetches, exactly as a
   build would.

   Returns a molto_exit_code. */
[[nodiscard]] int metadata_command_run(const char *output_path, bool include_dev);

#endif /* MOLTO_METADATA_COMMAND_H */
