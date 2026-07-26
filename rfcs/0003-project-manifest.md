# RFC 0003: Project Manifest

- RFC Number: 0003
- Title: Project Manifest
- Status: Draft
- Created: 2026-07-26

## Summary

This RFC specifies the schema of `Project.toml`, the single source of truth
for a Molto project (see RFC-0001, Vision, and `spec.md` section 5).

## Motivation

Dependency management, build profiles, and artifact type all need one
authoritative, machine-readable file so that `molto` (RFC-0002) and third
party tools can operate on a project without guessing conventions.

## File Location

Every project has exactly one `Project.toml` at its root. Commands search
the current directory and its ancestors to locate it, mirroring the
convention-over-configuration philosophy in RFC-0001.

## Schema Overview

```toml
[package]
name    = "my_app"
version = "0.1.0"
artifact = "static"   # source | static | shared, default: static

[deps]
yyjson = "1.2.32"

sqlite = {
    git = "https://github.com/sqlite/sqlite",
    tag = "3.50.0"
}

http = {
    path = "modules/http"
}

[profile.debug]
opt_level = 0
debug_info = true

[profile.release]
opt_level = 3
debug_info = false

[profile.bench]
opt_level = 3
debug_info = false

[profile.custom]
opt_level = 2
debug_info = true
```

## `[package]`

| Key        | Type   | Required | Description                                   |
|------------|--------|----------|------------------------------------------------|
| `name`     | string | yes      | Package identifier, `snake_case`                |
| `version`  | string | yes      | Semantic version                                |
| `artifact` | string | no       | `source`, `static`, or `shared` (default `static`, see `spec.md` section 9) |

## `[deps]`

Each entry maps a dependency name to one of the following forms:

- **Registry**: a plain version string, e.g. `yyjson = "1.2.32"`.
- **Git**: `{ git = "<url>", tag = "<tag>" }` (or `branch` / `rev`).
- **Local path**: `{ path = "<relative-path>" }`.
- **Archive**: `{ archive = "<url>" }`.
- **Recipe**: `{ recipe = "<name>" }`, resolved through a registry-hosted
  Recipe (`spec.md` section 8).

Exactly one source key (`git`, `path`, `archive`, `recipe`) may be present
per dependency, or none for a plain registry version string.

## `[profile.*]`

One table per build profile: `debug`, `release`, `bench`, or a user-defined
`custom` name (`spec.md` section 13). `release` enables compiler
optimizations by default. Recognized keys:

| Key          | Type    | Description                          |
|--------------|---------|----------------------------------------|
| `opt_level`  | integer | Compiler optimization level            |
| `debug_info` | bool    | Whether to emit debug symbols          |

Unrecognized keys are passed through to the active compiler backend
unchanged, preserving compiler-agnosticism (RFC-0001, Non-Goals).

## Immutability and Discovery

`Project.toml` is hand-edited or modified via `molto add` / `molto remove`
(RFC-0002). It is never generated or overwritten silently; Molto only writes
to it in response to explicit user commands. Everything else about the
project — source layout, tests, benchmarks — is discovered from the
filesystem per RFC-0001.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md)
