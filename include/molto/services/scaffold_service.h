#ifndef MOLTO_SCAFFOLD_SERVICE_H
#define MOLTO_SCAFFOLD_SERVICE_H

/* Create the project layout (src/, tests/, Project.toml) under `root`
   for a package named `name`. `root` may be "." for the current directory.
   Returns a molto_exit_code. */
[[nodiscard]] int scaffold_project(const char *root, const char *name);

#endif /* MOLTO_SCAFFOLD_SERVICE_H */
