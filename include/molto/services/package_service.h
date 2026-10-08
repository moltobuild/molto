#ifndef MOLTO_PACKAGE_SERVICE_H
#define MOLTO_PACKAGE_SERVICE_H

#include <molto/services/recipe_service.h>
#include <molto/util/str_list.h>

#define PACKAGE_PRUNE_STAMP "package-prune-1\n"

/* Shared author/consumer validation and manifest-to-artifacts conversion. */
[[nodiscard]] bool package_read(const char *root, const char *name, project_ctx *ctx,
                                recipe_artifacts *artifacts, char *err, size_t err_size);
/* Select relative files from a supplied inventory, using the store's allowlist. */
[[nodiscard]] bool package_select(const project_ctx *ctx, const str_list *files, str_list *kept,
                                  char *err, size_t err_size);
/* Only called on fetched working trees, before their atomic installation. */
[[nodiscard]] bool package_prune(const char *root, char *err, size_t err_size);
[[nodiscard]] int package_command_run(bool list_only);

#endif
