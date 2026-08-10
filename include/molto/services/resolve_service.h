#ifndef MOLTO_RESOLVE_SERVICE_H
#define MOLTO_RESOLVE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/project_deps.h>
#include <molto/services/recipe_service.h>
#include <molto/services/source_service.h>

/*
 * Turning a registry coordinate into something Molto can act on.
 *
 * `sqlite = "3.53.4"` in a manifest says which package and which version, and
 * nothing about where the bytes are — that lives in the recipe, published once.
 * This is the step in between: ask the registry, and come back with the
 * `[source]` the fetcher takes and the `[artifacts]` a compile line needs.
 *
 * One request. `GET /v1/packages/{name}/{version}` lists every target
 * published for that version and each one carries its whole recipe (RFC-0010),
 * so the target is chosen here from what was actually published rather than
 * guessed in the URL. Molto has no platform triple of its own to guess with:
 * that vocabulary belongs to pickup, and a second one here would disagree with
 * it the first time they met.
 *
 * Nothing in `resolved_dep` points into the response; the JSON is freed before
 * it returns.
 */

typedef struct {
    recipe_coordinate coordinate;
    /* Where to fetch from, when the recipe's form is source. */
    source_spec source;
    recipe_artifacts artifacts;
    /* What this package depends on in turn, read from the same recipe. It is
       here because it is only readable while the registry's answer is alive,
       and because a second request to learn it would ask the registry the
       question it already answered. */
    project_deps deps;
    /* Binary form only, and empty for a source recipe, which has no bytes in
       the registry at all. */
    char download_url[SOURCE_URL_MAX];
    char checksum[SOURCE_DIGEST_MAX];
} resolved_dep;

/* Ask `base_url` for `name` at exactly `version`. */
[[nodiscard]] bool resolve_version(const char *base_url, const char *name, const char *version,
                                   resolved_dep *out, char *err, size_t err_size);

/* The same, from a body already in hand: the release JSON the endpoint above
   answers with.

   Split out because everything worth getting wrong is here — choosing a
   target, refusing a yanked artifact, reading the recipe, checking that the
   registry answered about what was asked — and none of it needs a network to
   exercise. */
[[nodiscard]] bool resolve_read_release(const char *body, const char *name, const char *version,
                                        resolved_dep *out, char *err, size_t err_size);

#endif /* MOLTO_RESOLVE_SERVICE_H */
