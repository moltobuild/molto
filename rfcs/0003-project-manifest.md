# RFC 0003: Project Manifest

- RFC Number: 0003
- Title: Project Manifest
- Status: Draft
- Created: 2026-07-26

## Summary

This RFC specifies the schema of `Project.toml`, the single source of truth for
a Molto project (see RFC-0001, Vision, and `spec.md` section 5). It defines the
`[package]`, `[target]`, `[env]`, `[profile.*]`, `[registries]` and `[deps]`
tables.

## Motivation

Package identity, the build configuration (compiler, language standard, link
libraries), build profiles, environment, registries and dependencies all need
one authoritative, machine-readable file so that `molto` (RFC-0002) and third
party tools can operate on a project without guessing conventions.

## File Location

Every project has exactly one `Project.toml` at its root. Commands search the
current directory and its ancestors to locate it, mirroring the
convention-over-configuration philosophy in RFC-0001.

## Schema Overview

```toml
[package]
name     = "my_app"
version  = "0.1.0"
artifact = "static"        # source | static | shared, default: static

[target]
compiler = "gcc"           # gcc | g++ | clang | llvm | msvc
std      = "c23"           # C standard passed as -std=
cpp_std  = "c++20"         # C++ standard for C++ translation units
link     = ["m", "pthread"] # system libraries: -lm -lpthread

[env]
MOLTO_LOG = "debug"        # injected into compilation and execution

[registries]
myorg = "https://packages.myorg.dev"

[deps]
yyjson = "1.2.32"

sqlite = { git = "https://github.com/sqlite/sqlite", tag = "3.50.0" }

http = { path = "modules/http" }

fast_json = { version = "2.0.0", registry = "myorg", optional = true }

engine = { version = "3.1.0", features = ["vulkan"], default_features = false }

[profile.debug]
opt_level  = 0
debug_info = true

[profile.release]
opt_level  = 3
debug_info = false

[profile.bench]
opt_level  = 3
debug_info = false

[profile.custom]
opt_level  = 2
debug_info = true
```

## `[package]`

| Key        | Type   | Required | Description                                                                 |
|------------|--------|----------|-----------------------------------------------------------------------------|
| `name`     | string | yes      | Package identifier, `snake_case`                                            |
| `version`  | string | yes      | Semantic version                                                            |
| `artifact` | string | no       | `source`, `static`, or `shared` (default `static`, see `spec.md` section 9) |

Publishing metadata (`description`, `license`, `authors`, `repository`, …) is
**reserved** for a future revision (see Reserved Sections).

## `[target]`

Build configuration, kept compiler-agnostic (RFC-0001, Non-Goals): Molto
orchestrates the toolchain, it does not replace it.

| Key        | Type          | Required | Description                                                                                          |
|------------|---------------|----------|------------------------------------------------------------------------------------------------------|
| `compiler` | string        | no       | Toolchain to orchestrate: `gcc`/`g++` (GCC), `clang`/`llvm` (LLVM), `msvc`. Absent → autodetected.  |
| `std`      | string        | no       | C standard, e.g. `"c23"`, `"c17"`, `"c11"`; translated to `-std=`. Absent → compiler default.        |
| `cpp_std`  | string        | no       | C++ standard for C++ translation units, e.g. `"c++20"`.                                              |
| `link`     | array[string] | no       | System libraries to link, e.g. `["m", "pthread"]` → `-lm -lpthread`.                                 |

Molto selects the correct C vs C++ driver per source file: for `compiler = "gcc"`
it compiles `.c` with `gcc` and `.cpp`/`.cc` with `g++`; likewise `clang`/`clang++`
for LLVM. Toolchain availability follows the roadmap (`spec.md` section 19): GCC
on Linux in v0.1, Clang in v0.2, MSVC afterwards.

The keys `flags`, `defines`, `include` and a cross-compilation `triple` are
**reserved** for a future revision.

## `[env]`

A table of `KEY = "value"` pairs. Each variable is injected both into the
environment of the compiler/linker invocations and into the execution of the
program under `molto run` and `molto test`. Values are strings.

## `[profile.*]`

One table per build profile: `debug`, `release`, `bench`, or the user-defined
`custom` name (`spec.md` section 13). `release` enables compiler optimizations
by default.

| Key          | Type    | Description                    |
|--------------|---------|--------------------------------|
| `opt_level`  | integer | Compiler optimization level    |
| `debug_info` | bool    | Whether to emit debug symbols  |

Unrecognized keys are passed through to the active compiler backend unchanged,
preserving compiler-agnosticism. The keys `lto`, `strip`, `sanitizers` and
`warnings_as_errors` are **reserved** for a future revision.

## `[registries]`

A map of registry name to URL:

```toml
[registries]
myorg = "https://packages.myorg.dev"
```

Dependencies select a named registry with `registry = "<name>"`. When omitted, a
dependency resolves against the official public registry. The registry protocol
is public and backend-agnostic (`spec.md` sections 15-16).

## `[deps]`

Each entry maps a dependency name to either a plain version string (shorthand
for a registry dependency) or an inline table. In table form, exactly **one**
source key must be present:

| Key                | Type          | Description                                                                    |
|--------------------|---------------|--------------------------------------------------------------------------------|
| `version`          | string        | Semver requirement; resolved from a registry (source)                          |
| `git`              | string        | Git repository URL (source); pair with one of `branch`/`tag`/`rev`             |
| `branch`           | string        | Git branch (with `git`)                                                        |
| `tag`              | string        | Git tag (with `git`)                                                           |
| `rev`              | string        | Git commit revision (with `git`)                                               |
| `path`             | string        | Local directory (source)                                                       |
| `archive`          | string        | Archive URL (source)                                                           |
| `recipe`           | string        | Recipe name, resolved through a registry (source; `spec.md` section 8)         |
| `registry`         | string        | Named registry to use (default: the official one)                              |
| `artifact`         | string        | How the dependency is built: `source`/`static`/`shared`                        |
| `optional`         | bool          | If true, only resolved when a feature enables it (default false)               |
| `features`         | array[string] | Features to enable in the dependency                                           |
| `default_features` | bool          | Whether to enable the dependency's default features (default true)            |

Rules:

- Exactly one source among `version`, `git`, `path`, `archive`, `recipe`. The
  plain-string form `dep = "1.2.3"` is equivalent to `{ version = "1.2.3" }`.
- `branch`, `tag` and `rev` are only valid alongside `git`, and at most one of
  them may be present.
- Source repositories are never cached by Molto; only reusable build artifacts
  are (`spec.md` sections 9-10).

## Immutability and Discovery

`Project.toml` is hand-edited or modified via `molto add` / `molto remove`
(RFC-0002). It is never generated or overwritten silently; Molto only writes to
it in response to explicit user commands. Everything else about the project —
source layout, tests, benchmarks — is discovered from the filesystem per
RFC-0001.

## Reserved Sections

The following are acknowledged directions but intentionally **not** specified
yet, so the format can grow without breaking:

- `[features]` — the package's own feature flags and what they enable.
- `[dev-deps]` / `[build-deps]` — dependencies used only for tests/benches or
  for build scripts.
- `[package]` publishing metadata (`description`, `license`, `authors`, …).
- `[workspace]` — multi-package workspaces (`members`).
- A manifest schema/version key for forward compatibility.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md)
