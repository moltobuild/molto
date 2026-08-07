# RFC 0008: Dependency Resolution

- RFC Number: 0008
- Title: Dependency Resolution
- Status: Draft
- Created: 2026-08-07

## Summary

This RFC specifies how Molto turns the `[deps]` table of a manifest into a
concrete, reproducible set of dependencies on disk: what each of the five
sources means, how a version requirement is matched, how conflicting
requirements are resolved, what is written to the lock file, and what is
cached.

It does **not** redefine `[deps]`. RFC-0003 already fixes the format, the keys
and their validity rules, and that table is the input to this document rather
than part of it. What RFC-0003 deliberately left open is the algorithm, and
that is what follows.

## Motivation

`[deps]` is decorative today. A manifest can declare dependencies and nothing
happens: the manifest reader binds `[package]`, `[target]`, `[test]`, `[env]`
and `[profile.*]`, and never looks at `deps` or `[registries]`. `molto add`,
`molto remove` and `molto update` exit with the "not implemented" code that
RFC-0002 reserves for exactly this.

Worse, the canonical form is unreadable. The manifest parser recognises an
inline table and **discards the key in silence**, so
`http = { path = "modules/http" }` parses successfully and yields nothing. The
multi-line form shown in `spec.md` §7 does not even get that far: the parser
never enters inline-table handling, reads `sqlite = {` as a value it cannot
parse, and fails the whole manifest. A specification that ignores this would be
specifying an algorithm whose input cannot be read.

Everything else in Molto is blocked behind this. There is no reason to build a
global artifact cache (RFC-0007) with nothing to put in it, no reason for the
build graph to grow edges with no packages to order, and no reason for a
registry client to read a catalogue nobody consumes.

## Precondition: the manifest must be readable

Two gaps in `src/util/toml.c` are prerequisites, not implementation detail, and
they belong in this RFC because they define the boundary of its input:

- **Inline tables must parse.** `{ git = "…", tag = "3.50.0" }` is the primary
  form of a dependency in RFC-0003. Skipping it silently is the failure mode
  this whole ecosystem is meant to avoid: the user wrote a dependency, Molto
  read the file without complaint, and the dependency is not there. Until inline
  tables parse, an unparsed one **MUST** be an error rather than a skip.
- **The manifest must be writable.** `molto add` and `molto remove` modify
  `Project.toml` (RFC-0002), and there is no TOML writer. The writer must
  preserve comments, key order and formatting outside the table it edits;
  RFC-0003 promises the file is never "generated or overwritten silently", and
  a writer that reformats a hand-edited manifest breaks that promise even when
  the semantics survive.

Reading unknown keys of a table is already possible — the parser exposes the
declaration-ordered key list that a `[deps]` reader needs — so the shape of
`[deps]` is not the problem. The values are.

## The five sources

Every dependency has exactly one source (RFC-0003). What follows is what each
one fetches and what makes it deterministic.

### `version` — the registry

The default. A name and a semver requirement are resolved against a registry
(RFC-0010) to a single exact version, whose artifact is downloaded and verified
against the checksum the registry reports. `registry = "<name>"` selects a
registry declared in `[registries]`; without it, the official one is used.

### `git`

A repository URL, paired with at most one of `branch`, `tag` or `rev`. All three
resolve to a **commit id**, and it is the commit id that is recorded: a branch
is not a version, and `main` today is not `main` next week. A dependency
declared by branch is therefore reproducible for everyone who shares the lock
file, and moves only when `molto update` is run.

Molto clones; it does not vendor. The working copy lives outside the project and
is never committed.

### `path`

A local directory, relative to the manifest that declares it. It is the source
for a dependency being developed alongside its consumer, and it is the one
source with no version and no integrity check — the bytes are whatever is on
disk right now, which is the point.

A path dependency is **never cached and never published**. A package whose
`[deps]` contains a `path` entry cannot be published to a registry, because the
path means nothing on another machine.

### `archive`

A URL to a source archive. Because a URL can serve different bytes tomorrow, an
archive dependency **MUST** carry a checksum; without one there is no difference
between "the upstream re-rolled the tarball" and "someone replaced it".

### `recipe`

A recipe name resolved through a registry. The recipe describes how the
dependency is obtained and built (RFC-0009); this source exists for the
libraries that were never designed to be consumed as packages — the ones with
a `configure` script and thirty years of history. The recipe is the adapter.

## Versions

Versions are semver: `major.minor.patch`, with optional pre-release and build
metadata, ordered by semver precedence. Build metadata never affects ordering or
matching. A version string that is not semver is a resolution error rather than
a string compared byte by byte.

The bare form is a **caret requirement**: `dep = "1.2.3"` means "at least 1.2.3
and below 2.0.0", and for `0.x` versions the compatibility boundary is the minor
component, since a zero major promises nothing. This is the Cargo convention and
it is chosen because it is the one the ecosystem this project imitates has
already taught people.

Explicit operators are accepted: `=`, `>=`, `>`, `<`, `<=`, `~` and `^`, and a
comma-separated list is a conjunction. `*` matches anything and is discouraged
in published packages for the reason every unbounded requirement is
discouraged: it makes the dependency's next release the publisher's problem.

Resolution picks the **highest version that satisfies every requirement**,
never the lowest and never the newest available. Picking the highest satisfying
version means a new release is adopted by re-resolving rather than by editing
manifests; picking it *within* the constraints means it is never adopted by
surprise.

Version comparison lives in Molto, not in the registry. The registry treats a
version as an opaque string and serves exact coordinates only (RFC-0010), which
keeps the protocol free of a semver dialect and lets a private registry be
implemented by anyone who can serve JSON.

## One version per dependency

This is the decision that separates a C package manager from a Rust one.

Cargo resolves conflicting requirements by keeping both: `serde 1.4` and
`serde 2.0` coexist in the same binary because Rust mangles each crate's symbols
with a distinct hash, so the two never collide. That mechanism does not exist
here. The C linker has one flat namespace. Two copies of the same library in
one link produce duplicate symbols if you are lucky, and if you are not, they
produce a binary in which allocations from one copy are freed by the other and
every struct layout is a coin flip.

Therefore:

> **A dependency appears exactly once in a resolved graph.** Requirements from
> different dependents on the same package are unified into one requirement, and
> one version satisfies all of them or resolution fails.

Unification is intersection. If `a` needs `png ^1.2` and `b` needs `png ^1.5`,
the intersection is `^1.5` and the highest matching version is chosen. If `b`
needs `png ^2.0`, the intersection is empty and **resolution fails** with exit
code 3 (RFC-0002), printing both requirement chains — who asked for what, and
through which dependency — because the user's next action is to change one of
them and they need to know which.

Molto does not resolve this by vendoring, renaming or namespacing. Each of those
is a real technique and each of them is a compiler feature Molto does not have,
which puts them outside the boundary of RFC-0001.

## The lock file

Resolution writes `Molto.lock` at the workspace root. It records, for every
package in the resolved graph, the exact version, the exact source (including
the resolved commit id for a `git` dependency), the integrity checksum, and the
dependents that pulled it in.

`Molto.lock` is **generated and committed**. It is generated because a
hand-edited lock file is a lock file that lies. It is committed because that is
the only thing that makes a build reproducible on a second machine.

The obvious alternative is the WSDB, and it is wrong. The WSDB is local,
disposable, and `.gitignore`d by design (RFC-0004) — deleting it must never do
more than force a rebuild. A resolution stored there would be re-derived on
every clone, on every CI run, and on every teammate's machine, which is to say
it would not be a resolution at all but a fresh guess each time, taken against
whatever the registry happens to serve that day. Reproducibility is a stated
goal, and it is a property of a file that travels.

The format is TOML with an array of tables, which the parser already reads:

```toml
version = 1

[[package]]
name = "yyjson"
version = "0.10.0"
source = "registry+https://molto-registry.example.dev"
checksum = "sha256:…"
dependencies = []
```

A leading `version` key covers the lock format itself. An unknown lock version
is discarded and the graph re-resolved, following the same fail-safe rule the
WSDB uses: never trust a file you cannot fully read.

## Determinism

Given the same manifests and the same lock file, resolution produces the same
graph — that is the easy half. Given the same manifests and *no* lock file, it
must also produce the same graph, and that requires care:

- Dependents are visited in a defined order, not in manifest-declaration order
  and not in the order network responses arrive.
- The candidate list for a package is sorted by semver precedence before
  selection, so "highest satisfying" is well-defined even when the registry
  returns versions unsorted.
- Failure is deterministic too: the same unsatisfiable graph reports the same
  conflict, not whichever one was noticed first.

`molto build` uses the lock file when it is present and consistent with the
manifest, and re-resolves only what the manifest changed. `molto update` is the
command that deliberately ignores the lock file's pins, within the manifest's
constraints (RFC-0002).

## What is cached and what is not

The rule is inherited from `spec.md` §9 and is worth restating because it is
counter-intuitive: **source repositories are never cached; reusable build
artifacts are.**

A cloned repository is large, is only useful at one exact revision, and is
something the user could have cloned themselves. What is worth keeping is what
was built from it, keyed by everything that went into the build — the source
identity, the flags, the profile, the toolchain — in the content-addressed
global cache described in RFC-0007. Two projects that depend on the same
version of the same library, built the same way, should compile it once.

Path dependencies are excluded from this entirely. Their bytes change without
notice, and a cache entry for a moving target is a stale entry waiting to
happen.

## Implementation Status

None of this is implemented. `[deps]` and `[registries]` are not read, there is
no dependency type, no semver comparator, no lock file, no fetcher for any of
the five sources, and no global cache. `molto add`, `molto remove` and
`molto update` exit with code 5 as RFC-0002 requires, which is the correct
behaviour for an unimplemented command and not a placeholder to be replaced by a
partial one.

The order the work has to happen in is fixed by dependencies between the pieces:
inline tables in the parser, then a `[deps]` reader, then the semver comparator,
then resolution against a single source (`path` is the one with no network),
then the lock file, then the registry client, then the global cache.

## Non-Goals

Molto does not resolve system libraries. `[target].link` names a `-l` and the
system linker finds it or does not; there is no attempt to model, version or
install what the platform already provides.

Molto does not build a dependency in a way its own manifest does not describe.
A registry dependency is a Molto package built like any other; anything with a
bespoke build is a recipe (RFC-0009), and the recipe names an existing build
system rather than a script.

## Reserved / Future

- `[features]`, `optional` and `default_features`. RFC-0003 reserves all three
  and no algorithm is specified here, because feature unification is a second
  resolution pass over a graph that does not exist yet — and because a feature
  no manifest can enable does not need an algorithm.
- `[dev-deps]` and `[build-deps]`. Both change which graph a command resolves,
  which is a question worth answering after one graph works.
- Vendoring — a command that copies the resolved graph into the project for
  offline or audited builds.
- Multi-package workspaces, where several members share one resolution and one
  lock file. Reserved by RFC-0004 for the same reason.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md) — `add`, `remove`, `update`, and exit code 3 for a failed resolution
- [RFC-0003: Project Manifest](0003-project-manifest.md) — the `[deps]` and `[registries]` tables this RFC consumes
- [RFC-0004: Workspace Specification](0004-workspace-specification.md) — why the resolution is not stored in the WSDB
- [RFC-0007: Build System](0007-build-system.md) — the build graph these edges extend, and the global cache
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — the `recipe` source
- [RFC-0010: Registry Specification](0010-registry-specification.md) — where a `version` source is resolved

See also `spec.md` sections 7 (Dependency Model), 9-10 (Artifacts and Global
Cache) and 15-16 (Registries).
