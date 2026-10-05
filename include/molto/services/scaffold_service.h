#ifndef MOLTO_SCAFFOLD_SERVICE_H
#define MOLTO_SCAFFOLD_SERVICE_H

#include <molto/services/manifest_service.h>

#include <stddef.h>

/* Create the project layout (src/, tests/, include/, Project.toml) under
   `root` for a package named `name`. A library gets include/<name>.h,
   src/<name>.c and a moltest suite in tests/, with moltest pinned to
   `moltest_tag`; a binary gets src/main.c and ignores it. `root` may be "."
   for the current directory. Returns a molto_exit_code. */
[[nodiscard]] int scaffold_project(const char *root, const char *name, project_kind kind,
                                   const char *moltest_tag);

/* The moltest release a new library starts on: the newest the repository
   tags, asked now, or MANIFEST_MOLTEST_KNOWN_TAG when it cannot be asked, said
   on stderr. Always a release, never a branch. */
void scaffold_newest_moltest_tag(char *out, size_t size);

#endif /* MOLTO_SCAFFOLD_SERVICE_H */
