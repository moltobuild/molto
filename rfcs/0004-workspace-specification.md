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
- **invalidate / prune** — drop entries for removed sources or stale targets.
- **close** — persist changes durably.

**Concurrency:** a workspace has a single writer at a time, guarded by a lock in
`.bin/`, so parallel `molto` invocations cannot corrupt the WSDB. Readers and the
build's internal parallelism (RFC on the task pool) operate within one holding
invocation.

## Project / Workspace Discovery

Molto locates the workspace root by walking upward from the current directory:

1. Start at the current working directory.
2. If it contains `Project.toml`, it is the workspace root.
3. Otherwise move to the parent directory and repeat.
4. If the filesystem root is reached with no `Project.toml`, report an error
   (not inside a Molto workspace).

The `.bin/` directory is created at the discovered root on first build. This
matches the manifest discovery described in RFC-0003. (Note: the current
implementation only checks the working directory; closing this gap is a small
change this RFC motivates.)

## Relationship to Current State

Until the WSDB is implemented, Molto keeps incremental state as sidecar files
under `build/`:

- `<object>.d` — header dependencies emitted by the compiler (`-MMD`).
- `<object>.cmd` / `<binary>.cmd` — the command fingerprint per artifact.

These are the interim "WSDB-lite". Introducing the WSDB will absorb them into
`.bin/`; behavior is unchanged until then.

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
