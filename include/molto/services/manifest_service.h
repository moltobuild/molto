#ifndef MOLTO_MANIFEST_SERVICE_H
#define MOLTO_MANIFEST_SERVICE_H

#include <stdbool.h>

/* Validate that `name` is a legal package identifier (snake_case, RFC-0001). */
[[nodiscard]] bool manifest_is_valid_name(const char *name);

/* Render a default Project.toml for a package named `name`.
   Returns a heap-allocated string the caller must free(), or NULL on error. */
[[nodiscard]] char *manifest_render_default(const char *name);

#endif /* MOLTO_MANIFEST_SERVICE_H */
