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

Today this information is scattered across sidecar files under `build/` (the
`.o.d` header-dependency files and the `.o.cmd` / `<binary>.cmd` command
fingerprints). That is an interim "WSDB-lite"; the WSDB consolidates it into one
versioned store.

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

The WSDB consolidates the interim sidecars (`.o.d`, `.o.cmd`, `<binary>.cmd`)
into one store.

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

*Current state:* the implementation only checks the working directory; a command
run from a subdirectory fails to find the manifest. Closing this gap (the upward
walk above) is a small, required change this RFC mandates.

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

*Current state:* Molto already recompiles and re-links correctly in these cases
(a deleted header fails the prerequisite check; a deleted source changes the
link command's fingerprint, forcing a re-link). What it does **not** yet do is
**prune** the orphaned objects/depfiles/fingerprints left under `build/`; the
WSDB will own that.

## Relationship to Current State

Until the WSDB is implemented, Molto keeps incremental state as sidecar files
under `build/`:

- `<object>.d` — header dependencies emitted by the compiler (`-MMD`).
- `<object>.cmd` / `<binary>.cmd` — the command fingerprint per artifact.

These are the interim "WSDB-lite". They drive correct recompiles and re-links,
but they are **not pruned** when a source or target is deleted, so orphaned
`.o`/`.d`/`.cmd` files accumulate under `build/`. Introducing the WSDB will
absorb these sidecars into `.bin/` and add the pruning described above; behavior
is otherwise unchanged until then.

## Reserved / Future

- Multi-package workspaces (`[workspace]` with `members`).
- The concrete WSDB binary schema and its migration story.
- Cross-invocation caching and integration with the registry (`spec.md`
  sections 15-16), targeted for v0.3.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md)
- [RFC-0003: Project Manifest](0003-project-manifest.md)

See also `spec.md` sections 4 (Workspace), 10 (Global Cache) and 11 (Workspace
Database).
