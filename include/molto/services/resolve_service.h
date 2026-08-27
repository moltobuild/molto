#ifndef MOLTO_RESOLVE_SERVICE_H
#define MOLTO_RESOLVE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/project_deps.h>
#include <molto/services/recipe_service.h>
#include <molto/services/registry_service.h>
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
    /* What its own sources need before they compile. Read here for the same
       reason as everything below: the recipe is alive now, and molto refuses a
       build system it cannot run before fetching a byte of the source. */
    recipe_build build;
    /* And what its own sources need copied into place before they compile. */
    recipe_provide provide;
    /* What this package depends on in turn, read from the same recipe. It is
       here because it is only readable while the registry's answer is alive,
       and because a second request to learn it would ask the registry the
       question it already answered. */
    project_deps deps;
    /* What the recipe says about itself, for the same reason: the answer is
       here now, and asking again later would be a request for something the
       registry has already sent. */
    manifest_about about;
    /* Binary form only, and empty for a source recipe, which has no bytes in
       the registry at all. */
    char download_url[SOURCE_URL_MAX];
    char checksum[SOURCE_DIGEST_MAX];
    /* The answer this was read from, verbatim, for resolve_remember. Empty
       when the answer came from disk — it is already there. */
    char body[REGISTRY_BODY_MAX];
} resolved_dep;

/* Ask `base_url` for `name` at exactly `version`. */
[[nodiscard]] bool resolve_version(const char *base_url, const char *name, const char *version,
                                   resolved_dep *out, char *err, size_t err_size);

/* The newest version published under `name`.
 *
 * The catalogue serves versions as opaque strings and in publication order
 * (RFC-0010), so the ordering happens here: which release is newest is a
 * semver question, and a registry that answered it would have to reimplement
 * Molto's comparator to give the same answer.
 *
 * A pre-release can be the newest, and is returned when it is. Whether to
 * write one into a manifest is the caller's call, not this function's. */
[[nodiscard]] bool resolve_latest_version(const char *base_url, const char *name, char *out,
                                          size_t out_size, char *err, size_t err_size);

/* Every version published under `name`, newest first.
 *
 * `resolve_latest_version` answers what to install; this answers what else
 * there is, which is what a search for a version that removes a conflict needs
 * (RFC-0008). Entries the comparator cannot order are dropped rather than
 * returned last: a caller here is choosing what to propose to a person, and
 * proposing something it could not order would be proposing a guess.
 *
 * One request, and the whole answer: `GET /v1/packages/{name}` lists every
 * release. */
[[nodiscard]] bool resolve_versions(const char *base_url, const char *name, str_list *out,
                                    char *err, size_t err_size);

/* The same, from a listing body already in hand. Split out for the reason
   `resolve_read_release` is: deciding which release is newest is worth testing
   without a server. */
[[nodiscard]] bool resolve_read_versions(const char *body, str_list *out, char *err,
                                         size_t err_size);

/* Answer from what the registry said last time, without asking again.
 *
 * Sound because a published coordinate is immutable (RFC-0010) and a manifest
 * names an exact version (RFC-0008): `sqlite 3.53.4` is the same artifact
 * today as yesterday, so the answer to a question nobody can change the answer
 * to is worth keeping. Without this, every build of every project asks the
 * registry to repeat itself.
 *
 * False when nothing is remembered, which is not an error: the caller asks.
 */
[[nodiscard]] bool resolve_remembered(const char *name, const char *version, resolved_dep *out);

/* The remembered answer itself, on the heap for the caller to free, or NULL
   when nothing is remembered. For a walk that reads the recipe rather than the
   resolution — the deps of a version it may never fetch. */
[[nodiscard]] char *resolve_release_body(const char *name, const char *version);

/* Keep the registry's answer beside the source it describes, so the next build
   can skip the request. Called after the fetch, because the fetch replaces the
   directory this writes into. A failure here is not a build failure — the next
   build just asks again — so it reports nothing. */
void resolve_remember(const char *name, const char *version, const char *body);

/* The same, from a body already in hand: the release JSON the endpoint above
   answers with.

   Split out because everything worth getting wrong is here — choosing a
   target, refusing a yanked artifact, reading the recipe, checking that the
   registry answered about what was asked — and none of it needs a network to
   exercise. */
[[nodiscard]] bool resolve_read_release(const char *body, const char *name, const char *version,
                                        resolved_dep *out, char *err, size_t err_size);

/* The same, for a caller that knows which platform it needs.

   `target_wanted` NULL is the line above: `any`, the only thing a dependency
   can be. Naming one selects that artifact instead, which is what installing a
   native executable requires — and what the target vocabulary belongs to
   pickup for (RFC-0014). An artifact published for other platforms and not
   this one is reported with what *was* published, because "no such plugin" and
   "not for your machine" are different problems. */
[[nodiscard]] bool resolve_read_release_for(const char *body, const char *name, const char *version,
                                            const char *target_wanted, resolved_dep *out, char *err,
                                            size_t err_size);

#endif /* MOLTO_RESOLVE_SERVICE_H */
