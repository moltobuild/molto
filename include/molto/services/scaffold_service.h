#ifndef MOLTO_SCAFFOLD_SERVICE_H
#define MOLTO_SCAFFOLD_SERVICE_H

#include <molto/services/manifest_service.h>

/* Create the project layout (src/, tests/, include/, Project.toml) under
   `root` for a package named `name`. A library gets include/<name>.h,
   src/<name>.c and a moltest suite in tests/; a binary gets src/main.c.
   `root` may be "." for the current directory. Returns a molto_exit_code. */
[[nodiscard]] int scaffold_project(const char *root, const char *name, project_kind kind);

#endif /* MOLTO_SCAFFOLD_SERVICE_H */
