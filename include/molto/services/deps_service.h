#ifndef MOLTO_DEPS_SERVICE_H
#define MOLTO_DEPS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/project_ctx.h>
#include <molto/services/dep_graph.h>
#include <molto/util/str_list.h>

/*
 * From `[deps]` to something a build can use.
 *
 * This is the step between the manifest and the compiler: every dependency is
 * resolved (a registry answers, or the manifest already said where the bytes
 * are), fetched into the shared cache, and reduced to the four things a
 * compile line and a link line need.
 *
 * The result is flat on purpose. A build does not care which dependency
 * contributed which `-I`; it needs the union, in order. Keeping per-dependency
 * structure here would also mean an array of `recipe_artifacts`, which at
 * thirty-two entries is half a megabyte of struct for information nothing
 * downstream asks for.
 *
 * Paths come out absolute — sources and include directories both — because a
 * dependency lives in the cache and not under the project root, and the build
 * anchors a relative path at the root.
 */

typedef struct {
    str_list sources;  /* .c files the consumer compiles as its own */
    str_list includes; /* -I directories */
    str_list defines;  /* -D */
    str_list flags;    /* passed verbatim */
    str_list links;    /* -l */
} prepared_deps;

void prepared_deps_init(prepared_deps *out);
void prepared_deps_free(prepared_deps *out);

/* Resolve, fetch and reduce every dependency `ctx` declares. An empty `[deps]`
   succeeds with everything empty and touches no network.

   A dependency that names a version is resolved against a registry: the one
   `[registries]` names for it, or the one `molto login` stored, or the official
   one. A dependency that carries its own source is fetched directly, and must
   bring a `recipe.toml` at the root of what was fetched — `[deps]` has nowhere
   to say what to compile, so the source has to say it itself. */
[[nodiscard]] bool deps_prepare(const project_ctx *ctx, prepared_deps *out, char *err,
                                size_t err_size);

/* The same, from a graph already resolved: everything that ships. Split out
   because everything here is filesystem work on a graph that is already in
   hand, and a caller that also wants to write a lock file should not resolve
   twice to get both. */
[[nodiscard]] bool deps_prepare_graph(const dep_graph *graph, prepared_deps *out, char *err,
                                      size_t err_size);

/* What the test build adds on top: the packages reachable only through
   `[dev-deps]`. Their include directories never reach the command line that
   compiles `src/`, which is what makes the separation real rather than
   documented — a source that includes one fails to compile, on the first
   build, with "no such file" (RFC-0008). */
[[nodiscard]] bool deps_prepare_dev(const dep_graph *graph, prepared_deps *out, char *err,
                                    size_t err_size);

#endif /* MOLTO_DEPS_SERVICE_H */
