#ifndef MOLTO_IR_TRANSFORM_H
#define MOLTO_IR_TRANSFORM_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/services/deps_service.h>
#include <molto/services/ir_service.h>

/*
 * Transforms: a document in, a document out (RFC-0015).
 *
 * A transform is given the whole document and returns the whole document, and
 * that shape is the point rather than a convenience. Two transforms compose
 * into a transform — the order is a list, the result is inspectable at every
 * step, and "run A then B" needs no coordination between A and B. Two hooks on
 * one point do not compose into anything: they both fire, in an order somebody
 * has to define, mutating shared state, and whether the result is correct
 * depends on what each did to what the other was expecting.
 *
 * A transform is also pure with respect to the filesystem. It reads the
 * document and returns a document; it does not write files and it does not
 * resolve anything. What it needs, it is handed.
 *
 * There is one of them so far, and this header is where the list will live.
 */

/* Say in the document what the resolved graph found.
 *
 * `resolve` produces the graph (RFC-0015), and it is the one phase closed to
 * plugins: a plugin that could influence which versions a build uses would make
 * a lock file a suggestion. What the phase produces still has to reach the
 * document, and this is what puts it there — one `Dependency` node per package
 * that ships sources, each carrying the coordinate it resolved to, where its
 * bytes are on this machine, and the interface it exports.
 *
 * It adds and never replaces: a document that already names dependencies keeps
 * them, and these go on the end. Nothing calls it twice today, and a transform
 * that silently dropped what an earlier one wrote would be a composition rule
 * nobody could reason about.
 *
 * The two sets are described together because the node says which it is: a
 * runtime package is scoped `runtime` and a development one `dev`, and the fold
 * below reads that back rather than being told again. `dev` may be NULL, which
 * is a project with no `[dev-deps]`.
 *
 * What it deliberately does not do is fold that interface into the targets'
 * scopes. That is a second transform, and it belongs with the change that makes
 * the engine read a target's options from here — writing it before then would
 * mean two implementations folding the same thing, of which the engine reads
 * the other one.
 *
 * False with a message in `err`; `doc` is left as it was found. */
[[nodiscard]] bool ir_transform_dependencies(ir_document *doc, const prepared_deps *deps,
                                             const prepared_deps *dev, char *err, size_t err_size);

/* Fold what the dependencies export into the targets that compile against them.
 *
 * This is the transform `merge_deps` always was. Its own comment made the
 * argument before transforms existed: a dependency's includes, defines, flags
 * and libraries are exactly the things a target already carries, so folding
 * them means nothing downstream has to learn what a dependency is.
 *
 * Runtime dependencies reach every target. Development dependencies reach the
 * test targets and no others, which is what makes the separation real rather
 * than documented: a source under `src/` that includes one fails to compile, on
 * the first build, with "no such file" (RFC-0008).
 *
 * It takes nothing but the document. Everything it needs is already in there:
 * `ir_transform_dependencies` wrote one node per package and said which scope
 * each is, so the fold reads the document rather than being handed the same
 * facts a second time. That is what a transform of RFC-0015 is — a document in,
 * a document out — and it is what makes a published document reproducible: a
 * consumer that has only the bytes can fold them exactly as the engine did.
 *
 * Appended after what the manifest named, in the order a command line receives
 * them, so folding does not reorder anything a target already carried.
 *
 * False with a message in `err`. */
[[nodiscard]] bool ir_transform_fold_dependencies(ir_document *doc, char *err, size_t err_size);

#endif /* MOLTO_IR_TRANSFORM_H */
