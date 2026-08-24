#ifndef MOLTO_IR_SERVICE_H
#define MOLTO_IR_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <molto/util/doc.h>
#include <molto/util/str_list.h>

/*
 * The Build Intermediate Representation (RFC-0013): what is to be built, said
 * independently of who described it.
 *
 * A document is a `Project` and everything reachable from it. Molto's own
 * manifest produces one, and so does a frontend plugin reading a `meson.build`,
 * and the point of the exercise is that the two are the same document. A
 * representation whose own author's tools do not produce it is one nobody has
 * tested.
 *
 * This revision carries eight of RFC-0013's ten node types. `BuildStep` and
 * `GeneratedSource` are absent, and their absence is a refusal rather than a
 * gap: both lower to a command, both need the `generator` capability that no
 * plugin can yet be granted, and RFC-0015 has not settled what a generate phase
 * does to the freshness model. Since an unknown node type is fatal by
 * specification, a reader that accepted them and could not run them would have
 * to reject the document anyway — so it rejects it by name, at the node, rather
 * than somewhere further in.
 *
 * Nothing here is fixed-width. RFC-0013 is explicit that the ninety-five
 * character cap a hand-written manifest option carries MUST NOT apply to a
 * document a machine produced: the command line of a Meson `custom_target` and
 * a path into the global cache both exceed it, and a truncated option is a
 * green build of something else.
 */

/* This revision. A document declares it, and a plugin declares the one it
   speaks in its recipe, so a mismatch is a refusal before the process starts
   rather than a half-read document (RFC-0014). */
#define IR_SCHEMA 1

/* Where an option sits in the compile line RFC-0007 composes. The order the
   three reach a command line is contract, not detail: it is the fingerprint,
   and a compiler takes the last of two contradictory flags. Carrying the scope
   on the node is what lets a producer add an option without knowing where in
   the line it belongs. The engine orders by scope; a producer's array order is
   preserved only within a scope. */
typedef enum {
    ir_scope_target,
    ir_scope_profile,
    ir_scope_unit,
} ir_scope;

typedef enum {
    ir_language_c,
    ir_language_cpp,
} ir_language;

/* `executable`, `static`, `shared`, `object` or `test`.
 *
 * `static` and `shared` are expressible and not yet buildable, on purpose:
 * RFC-0007 refuses a manifest asking for either because a `.a` needs `ar` and a
 * `.so` needs -fPIC, a soname and a versioning policy. That refusal moves here
 * rather than disappearing — the document may carry the node and the engine
 * reports it cannot build it — because a frontend for Meson, whose whole
 * vocabulary is libraries, has to be writable before Molto grows them. */
typedef enum {
    ir_target_executable,
    ir_target_static,
    ir_target_shared,
    ir_target_object,
    ir_target_test,
} ir_target_kind;

typedef enum {
    ir_dep_registry,
    ir_dep_path,
    ir_dep_git,
    ir_dep_archive,
} ir_dep_origin;

/* `CompileOption` and `LinkOption`: a value and the scope it applies at. One
   struct for both, because the two differ in which array they live in and in
   nothing else. */
typedef struct {
    char *value;
    ir_scope scope;
} ir_option;

/* `IncludePath`. `system` is what distinguishes -isystem from -I, and it is a
   field rather than a raw flag in `options` because a dependency's headers and
   a project's headers deserve different warning treatment, and because a raw
   flag would hide an include path from every consumer that wants to reason
   about include paths. */
typedef struct {
    char *value;
    ir_scope scope;
    bool system;
} ir_include;

/* `Source`: one translation unit, and the unit scope of RFC-0007. */
typedef struct {
    char *path;
    ir_language language;
    ir_option *options;
    size_t option_count;
} ir_source;

/* `Artifact`: what a target leaves behind. `install` is NULL when the document
   names none. */
typedef struct {
    ir_target_kind kind;
    char *path;
    char *install;
} ir_artifact;

/* `Target`: a thing that is built. This is the node RFC-0007 said was missing,
   and its arrival is what retires "there is exactly one executable per
   package".
 *
 * `depends_on` names targets, never files and never commands. It is the only
 * edge a producer may draw between two units of work, and a cycle in it is an
 * error at validation reported against the document — never a deadlock
 * discovered in the scheduler. */
typedef struct {
    char *name;
    ir_target_kind kind;
    ir_source *sources;
    size_t source_count;
    ir_option *options;
    size_t option_count;
    ir_include *includes;
    size_t include_count;
    ir_option *links;
    size_t link_count;
    str_list depends_on;
    ir_artifact artifact;
    bool has_artifact;
} ir_target;

/* `Dependency`: what the project builds against. `interface` is RFC-0009's
   `[artifacts]` in this document's vocabulary rather than a second one. */
typedef struct {
    char *name;
    char *version;
    ir_dep_origin origin;
    char *root;
    ir_include *includes;
    size_t include_count;
    ir_option *options;
    size_t option_count;
    ir_option *links;
    size_t link_count;
} ir_dependency;

/* `Project`, and the document it is the root of.
 *
 * `origin` is not provenance for a log: it selects the lowering rules of
 * ir_validate, and it is the reason a document carries it at all. `native` is
 * Molto's own frontend; anything else is the name of the plugin that produced
 * it, and is held to the stricter set.
 *
 * `files_read` is every file the frontend opened to produce this document, and
 * a frontend MUST report all of them. RFC-0013 makes it the invalidation key of
 * a cached document: a Meson frontend that reads `meson.build` and four
 * `subdir()` files and reports only the first has produced a cache entry that
 * is silently wrong. It is required from this revision even though nothing
 * caches yet, because adding it later would break every plugin already
 * written. */
typedef struct {
    long schema;
    char *name;
    char *version;
    char *root;
    char *origin;
    ir_target *targets;
    size_t target_count;
    ir_dependency *dependencies;
    size_t dependency_count;
    str_list files_read;
} ir_document;

/* The name Molto's own frontend puts in `origin`, and the one value ir_validate
   holds to the relaxed rules. */
#define IR_ORIGIN_NATIVE "native"

/* --- the model --- */

void ir_document_init(ir_document *doc);

/* Release a document and everything reachable from it. Safe on NULL and on an
   initialised-but-empty document, and safe twice. */
void ir_document_free(ir_document *doc);

/* True when this document came from a plugin rather than from `Project.toml`.
   The one question ir_validate asks about origin, in one place, so the two
   rule sets cannot disagree about which document they apply to. */
[[nodiscard]] bool ir_is_from_plugin(const ir_document *doc);

/* --- building one --- */

/* Every builder returns false on allocation failure and leaves the document
   usable — a half-added node is freed rather than kept, because a target with
   a NULL name is a document that describes nothing and reads like one that
   describes a target. */

[[nodiscard]] bool ir_set_project(ir_document *doc, const char *name, const char *version,
                                  const char *root, const char *origin);

/* Append a target and hand back a pointer to it. The pointer is valid until
   the next ir_add_target on the same document, which may move the array — so a
   caller filling several targets adds one, fills it, and only then adds the
   next. Returns NULL on allocation failure. */
[[nodiscard]] ir_target *ir_add_target(ir_document *doc, const char *name, ir_target_kind kind);

/* Likewise for a source inside a target. */
[[nodiscard]] ir_source *ir_add_source(ir_target *target, const char *path, ir_language language);

[[nodiscard]] bool ir_add_option(ir_option **array, size_t *count, const char *value,
                                 ir_scope scope);
[[nodiscard]] bool ir_add_include(ir_include **array, size_t *count, const char *value,
                                  ir_scope scope, bool system);

[[nodiscard]] bool ir_set_artifact(ir_target *target, ir_target_kind kind, const char *path,
                                   const char *install);

[[nodiscard]] ir_dependency *ir_add_dependency(ir_document *doc, const char *name,
                                               const char *version, ir_dep_origin origin,
                                               const char *root);

/* --- the wire --- */

/* Write `doc` as JSON to `stream`.
 *
 * It carries `molto metadata`'s rule and for the same reason: no timestamp, no
 * serial, and two runs over one project produce one byte-identical document. A
 * dump that differs between runs cannot be diffed, and a document that cannot
 * be diffed cannot be reviewed or cached. */
[[nodiscard]] bool ir_write(const ir_document *doc, FILE *stream);

/* Read a document from either encoding — the JSON of the wire, or the TOML
 * somebody hand-wrote as a test fixture. One reader for both, which is the
 * argument doc_view already makes for recipes: two readers for one format
 * drift, and they drift asymmetrically, because the copy that only ever sees a
 * plugin's answers has no local file anyone can diff against.
 *
 * The directional rule of RFC-0013 is enforced here, and it is the most
 * consequential thing this function does:
 *
 *   an unknown attribute on a known node is ignored;
 *   an unknown node type is fatal, in both directions.
 *
 * Ignoring an attribute is safe because an attribute refines work that is
 * already described. Ignoring a node type is a green build of something that
 * was never asked for, which is the failure mode every RFC in this repository
 * is arranged around. `kind`, `language`, `scope` and a dependency's `origin`
 * are node vocabulary in that sense: a value this revision does not know is
 * refused, not defaulted.
 *
 * False with a message in `err`; `out` is left freed and empty. */
[[nodiscard]] bool ir_read(doc_view view, ir_document *out, char *err, size_t err_size);

/* Parse `text` as a JSON document and read it. What a frontend's answer goes
   through, and what a test reads a fixture with. */
[[nodiscard]] bool ir_read_json(const char *text, ir_document *out, char *err, size_t err_size);

/* --- validation --- */

/* The three places a path in a document is allowed to resolve inside. Absolute
   paths, and the caller's to supply: this service does not decide where a
   workspace is. `cache` may be NULL when there is none to allow. */
typedef struct {
    const char *workspace;
    const char *build_dir;
    const char *cache;
} ir_bounds;

/* Refuse a document that describes work Molto will not do on a producer's
 * behalf (RFC-0013).
 *
 * Permissions govern a plugin's process; they do not govern the document it
 * returns, and the document is executed by Molto, as the user, with the user's
 * privileges. A plugin denied the network can still return a `CompileOption` of
 * `-fplugin=/tmp/x.so`. **The sandbox decides what a plugin can touch, and this
 * decides what Molto will do on its behalf.** A design with only the first has
 * neither.
 *
 * Applied to every document: every path resolves inside `bounds`, target names
 * are unique, `depends_on` names a target in this document, and the graph is
 * acyclic. Applied only to a document from a plugin: options that load code
 * into the compiler, redirect the toolchain, or name an output are refused —
 * `Project.toml` is a file in the user's repository that their reviewer read
 * and their version control records, and a plugin's document is generated on
 * the fly by a binary fetched from a registry. The asymmetry is not a statement
 * about trust, and the day it stops being warranted is the day the first one
 * stopped being reviewable.
 *
 * False with a message naming the node and the rule. A caller reports it as a
 * diagnostic against the producer and exits exit_plugin_failure — never as a
 * build failure, because the build never started. */
[[nodiscard]] bool ir_validate(const ir_document *doc, const ir_bounds *bounds, char *err,
                               size_t err_size);

/* --- spelling --- */

/* The wire spelling of each enumerator, and the reverse. The pair exists so the
   writer and the reader cannot disagree about a name. */
[[nodiscard]] const char *ir_scope_name(ir_scope scope);
[[nodiscard]] const char *ir_language_name(ir_language language);
[[nodiscard]] const char *ir_target_kind_name(ir_target_kind kind);
[[nodiscard]] const char *ir_dep_origin_name(ir_dep_origin origin);

[[nodiscard]] bool ir_scope_from_name(const char *name, ir_scope *out);
[[nodiscard]] bool ir_language_from_name(const char *name, ir_language *out);
[[nodiscard]] bool ir_target_kind_from_name(const char *name, ir_target_kind *out);
[[nodiscard]] bool ir_dep_origin_from_name(const char *name, ir_dep_origin *out);

#endif /* MOLTO_IR_SERVICE_H */
