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
 * are), fetched into the shared cache, and reduced to what a compile line and a
 * link line need.
 *
 * The result comes out in two shapes because two different questions are being
 * asked. What the *consumer* compiles and links against is a union: it does not
 * care which dependency contributed which `-I`, only that all of them are
 * there. What each *dependency* compiles against is its own, and used to be
 * that same union — which meant one package's defines reached another
 * package's preprocessor, and no recipe could ask for a flag without every
 * other source in the build getting it too.
 *
 * Paths come out absolute — sources and include directories both — because a
 * dependency lives in the cache and not under the project root, and the build
 * anchors a relative path at the root.
 */

/* One dependency, and the command line its own sources compile with: the
   interface of every package it reaches, then its own, then what its recipe
   kept private.
 *
 * The lists are str_lists rather than a `recipe_artifacts` per package, which
 * is what makes keeping this structure affordable: a recipe_artifacts is
 * fifteen kilobytes of fixed-size arrays, and thirty-two of them is half a
 * megabyte to hold what a handful of strings holds. */
typedef struct {
    char name[DEP_NAME_MAX];
    /* What the package is, for anything that has to name it rather than
       compile it — a build's report, above all. The version is empty for an
       origin that carries none, which is a path dependency: its bytes are
       whatever is on disk. */
    char version[DEP_VERSION_MAX];
    dep_source origin;
    /* Where the fetched source sits, absolute. Every path below is under it,
       so it is what turns one of them back into the name the package's own
       author would use — and what a diagnostic quotes when the package is
       somewhere the reader can go and look. */
    char root[DEP_GRAPH_PATH_MAX];
    str_list sources;  /* .c files the consumer compiles as its own */
    str_list includes; /* -I directories, absolute */
    str_list defines;  /* -D */
    str_list flags;    /* passed verbatim */
    /* The standard its recipe asked for, per language. Empty means the recipe
       named none and the consumer's applies — which is what every package did
       before recipes could say. Unlike the lists above, these do not travel
       along the closure: a define is ABI a caller has to share, and a standard
       is not. */
    char std[RECIPE_STD_MAX];
    char cpp_std[RECIPE_STD_MAX];
} prepared_unit;

typedef struct {
    /* One per dependency that ships sources, in graph order. */
    prepared_unit *units;
    size_t unit_count;
    /* The interface of all of them together: what the consumer's own sources
       compile against, and what its link line carries. */
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
   build, with "no such file" (RFC-0008).

   A development dependency's own unit still sees whatever it reaches, runtime
   packages included: what brought a package into the graph says nothing about
   which headers its sources include. */
[[nodiscard]] bool deps_prepare_dev(const dep_graph *graph, prepared_deps *out, char *err,
                                    size_t err_size);

#endif /* MOLTO_DEPS_SERVICE_H */
