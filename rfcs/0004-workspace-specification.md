# RFC 0004: Workspace Specification

- RFC Number: 0004
- Title: Workspace Specification
- Status: Draft
- Created: 2026-07-29

## Summary

This RFC defines the **Workspace**: the project tree Molto operates on, its
Molto-owned metadata directory `.bin/`, the **Workspace Database (WSDB)** stored
inside it, the private **Workspace API** through which all access happens, and
the **discovery** algorithm that locates the workspace root. It formalizes
`spec.md` sections 4 and 11, and complements RFC-0003 (Project Manifest).

## Motivation

Molto needs authoritative, fast, incremental build state — the dependency
graph, the build graph, file hashes and timestamps, the exact command that
produced each artifact, and toolchain metadata. This state must be:

- **separate** from `Project.toml` (the human-edited source of truth, RFC-0003)
  and from `build/` (throwaway build outputs), and
- **machine-owned**: never hand-edited, safe to delete, and mutated only through
  a single sanctioned API so concurrent invocations cannot corrupt it.

This information used to be scattered across sidecar files under `build/` (the
`.o.d` header-dependency files and the `.o.cmd` / `<binary>.cmd` command
fingerprints). The WSDB consolidates it into one versioned store: the `.cmd`
sidecars are gone, and the compiler's `.o.d` files are absorbed into the store
and deleted as soon as they are read.

## The Workspace

A **workspace** is the directory tree rooted at the nearest ancestor of the
current directory that contains a `Project.toml`. There is exactly one workspace
per root. It contains:

- `Project.toml` — the single source of truth, human-edited (RFC-0003).
- `src/`, `tests/`, `bench/` — the conventional layout discovered from the
  filesystem (RFC-0001).
- `build/` — build outputs (objects, binaries), profile-scoped.
- `.bin/` — Molto-managed workspace metadata (this RFC).

Multi-package workspaces (a root that aggregates several member packages, the
`[workspace]` table reserved in RFC-0003) are out of scope here and specified in
a future revision.

## `.bin/`

`.bin/` lives at the workspace root and holds Molto's private metadata. It is:

- **owned by Molto**: never created or edited by hand or by third-party tools;
- **disposable**: deleting it is always safe — Molto regenerates it, at worst
  forcing a full rebuild;
- **local**: it should be listed in `.gitignore` (like `build/`).

It is distinct from `build/` (reproducible build artifacts) and from the global
cache `~/.molto/` (reusable artifacts shared across projects, `spec.md`
section 10). `.bin/` contains the WSDB and any future workspace-scoped metadata.

## WSDB (Workspace Database)

The WSDB is a single **binary, versioned** database and the authoritative source
of incremental truth. Conceptually it stores (`spec.md` section 11):

- **Dependency graph** — declared dependencies and their resolved sources.
- **Build graph** — translation units → objects → binary, plus test and bench
  targets.
- **File hashes and timestamps** — for sources and their headers.
- **Command fingerprints** — the exact compile/link command per artifact, so a
  change in flags/profile/toolchain triggers a rebuild.
- **Toolchain metadata** — compiler identity and version, target, language
  standard.

Invariants:

- Mutated **only** through the Workspace API; never hand-edited.
- Carries a **format-version** header. On a version mismatch or on detected
  corruption, Molto discards the WSDB and rebuilds from scratch (fail-safe:
  Molto never trusts an unreadable database, and never silently skips work).
- The concrete byte layout is implementation-defined and may evolve behind the
  version header; this RFC fixes the contents and invariants, not the encoding.

The WSDB replaced the interim sidecars (`.o.d`, `.o.cmd`, `<binary>.cmd`) with
one store.

### Resolved toolchains

Which compiler satisfies a manifest is the answer to a question put to an
external resolver (RFC-0003). The WSDB records it, so a build does not ask
again on every invocation.

The entry is keyed by language, fingerprinted by the request that produced it,
and holds the resolved drivers. The compiler itself is recorded as an **input**,
so the freshness machinery that watches source files also watches it.

It is resolved again when:

- the request changes — a different `requires`, `std`, vendor, or the arrival of
  C++ sources in a project that had none;
- the compiler it named is replaced or removed;
- `--refresh-toolchain` asks for it.

Installing a *newer* compiler does **not** invalidate it. A build that silently
changed compiler because something was installed would recompile everything and
possibly behave differently, without the project having changed. Choosing a new
one is an explicit act.

## Workspace API

All access to workspace metadata goes through the Workspace API — the only
sanctioned path (`spec.md` section 11). Third-party tools use this API, never
the raw file. Conceptually it offers:

- **open / create** — locate the workspace root (see Discovery) and load the
  WSDB, initializing an empty one if absent or version-incompatible.
- **query** — is a given unit up to date? read the build graph, the resolved
  dependency graph, and recorded toolchain metadata.
- **record** — a file's hash/timestamp, a command fingerprint, dependency edges,
  toolchain metadata.
- **invalidate / prune** — drop entries for removed sources, headers or stale
  targets (see Deletion and Pruning).
- **close** — persist changes durably.

**Concurrency:** a workspace has a single writer at a time, guarded by a lock in
`.bin/`, so parallel `molto` invocations cannot corrupt the WSDB. Readers and the
build's internal parallelism (RFC on the task pool) operate within one holding
invocation.

## Project / Workspace Discovery

Molto **MUST** locate the workspace root by walking upward from the current
directory, so commands work from any subdirectory of a project:

1. Start at the current working directory.
2. If it contains `Project.toml`, it is the workspace root.
3. Otherwise move to the parent directory and repeat.
4. If the filesystem root is reached with no `Project.toml`, report an error
   (not inside a Molto workspace).

The first directory found with a `Project.toml` wins (the nearest enclosing
project). The `.bin/` directory is created at that root on first build. This
matches the manifest discovery described in RFC-0003.

*Current state:* implemented. `workspace_find_root` performs the upward walk,
so `build`, `run` and `test` work from any subdirectory of a workspace.

## Deletion and Pruning

Removing a file from the project must not leave the build in a stale or
incorrect state. Molto handles the cases as follows:

- **A header is deleted.** Every translation unit whose recorded dependencies
  included that header is **recompiled**. The missing prerequisite forces the
  rebuild (fail-safe: a prerequisite that cannot be stat-ed is treated as
  changed). The compiler then errors or succeeds depending on whether the
  `#include` was also removed.
- **A source is deleted.** Filesystem discovery no longer yields it, so it drops
  out of the build graph; the affected target is **re-linked without its
  object**, and the WSDB **prunes** the now-orphaned entries (the object file,
  its depfile and its command fingerprint).
- **A target is deleted** (e.g. a removed test). Its binary and metadata stop
  being recorded and are pruned likewise.

Pruning orphaned outputs is possible precisely because the WSDB knows the
previous set of inputs and outputs; loose sidecar files do not make this easy.
When in doubt, Molto rebuilds rather than trusting stale state.

*Current state:* implemented. Recompiles and re-links were already correct (a
deleted header fails the prerequisite check; a deleted source changes the link
command's fingerprint). Pruning now covers both kinds of output — objects and
binaries — for sources and for tests, so a deleted test no longer leaves an
executable that `molto test` would keep running. Inputs are never deleted: they
are the user's files.

## Implementation Status

Implemented: `.bin/` with an exclusive `flock`, the magic + version header, the
fail-safe discard on a corrupt, truncated or version-incompatible file, the
atomic save (staging file + rename), the hybrid freshness signature
(nanosecond mtime + size, confirmed by a content hash), absorption of the
compiler's depfiles, and pruning of orphaned objects and binaries.

Not implemented yet, and each waiting on a feature rather than on this RFC:

- **The dependency graph.** The store has three entry kinds (input, object,
  binary) and no notion of a declared dependency, because dependency resolution
  does not exist yet.
- **Toolchain metadata.** The compiler's identity and version are only implicit
  inside the recorded command fingerprint; there is no entry to query them.
- **The build graph as a structure.** The store is a flat key/value map, not a
  model of targets. There is no bench target because there is no `molto bench`.
- **`invalidate`.** The API offers `prune`; explicit invalidation has not been
  needed.

## Reserved / Future

- Multi-package workspaces (`[workspace]` with `members`).
- The concrete WSDB binary schema and its migration story. A version bump
  currently discards the whole file and rebuilds, which is fail-safe and cheap;
  a real migration path is only worth it once a rebuild becomes expensive.
- Cross-invocation caching and integration with the registry (`spec.md`
  sections 15-16), targeted for v0.3.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md)
- [RFC-0003: Project Manifest](0003-project-manifest.md)
- [RFC-0006: Analysis Result Cache](0006-analysis-result-cache.md) — adds a fourth entry kind to the store described here
- [RFC-0007: Build System](0007-build-system.md) — what the build does with the state stored here
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — the dependency graph this store has no entry kind for, and why the resolution is not kept here

See also `spec.md` sections 4 (Workspace), 10 (Global Cache) and 11 (Workspace
Database).
