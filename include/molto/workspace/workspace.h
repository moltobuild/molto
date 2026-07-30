#ifndef MOLTO_WORKSPACE_H
#define MOLTO_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

/* Locate the workspace root by walking upward from the current directory until a
   directory containing Project.toml is found (RFC-0004). Writes the root path
   into `out` (`out_size` bytes). Returns false if no enclosing project exists. */
[[nodiscard]] bool workspace_find_root(char *out, size_t out_size);

#endif /* MOLTO_WORKSPACE_H */
