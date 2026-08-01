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
compiler = "gcc"            # preferred vendor; optional
std      = "c23"            # C standard passed as -std=
cpp_std  = "c++20"          # C++ standard for C++ translation units
link     = ["m", "pthread"] # system libraries: -lm -lpthread
defines  = ["NDEBUG"]       # -DNDEBUG (base for all profiles)
include  = ["vendor/include"] # -Ivendor/include
flags    = ["-fno-omit-frame-pointer"] # raw, verbatim
requires = ["attr_nodiscard"] # features that must really compile

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
flags      = ["-flto"]      # added on top of [target] for release only

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

**Current state:** `artifact` is **rejected** when declared. No kind changes
what gets built yet — every project links an executable, and `static`/`shared`
would require `ar`, `-shared` and `-fPIC` — so accepting the key and ignoring
it would misreport what Molto did. It is refused until it means something.

## `[target]`

Build configuration, kept compiler-agnostic (RFC-0001, Non-Goals): Molto
orchestrates the toolchain, it does not replace it.

| Key        | Type          | Required | Description                                                                                          |
|------------|---------------|----------|------------------------------------------------------------------------------------------------------|
| `compiler` | string        | no       | Preferred vendor: `gcc`, `clang`/`llvm`, `apple-clang`, `msvc`. Absent → any vendor may answer.      |
| `requires` | array[string] | no       | Compiler features the project needs, e.g. `["attr_nodiscard"]`. Proven, not assumed.                 |
| `std`      | string        | no       | C standard, e.g. `"c23"`, `"c17"`, `"c11"`; translated to `-std=`. Absent → compiler default.        |
| `cpp_std`  | string        | no       | C++ standard for C++ translation units, e.g. `"c++20"`.                                              |
| `link`     | array[string] | no       | System libraries to link, e.g. `["m", "pthread"]` → `-lm -lpthread`.                                 |
| `defines`  | array[string] | no       | Preprocessor defines, e.g. `["FOO=1", "NDEBUG"]` → `-DFOO=1 -DNDEBUG`.                                |
| `include`  | array[string] | no       | Extra include directories, e.g. `["vendor/include"]` → `-Ivendor/include`.                           |
| `flags`    | array[string] | no       | Raw, compiler-specific flags passed verbatim, e.g. `["-fno-omit-frame-pointer"]` (escape hatch).     |

`defines` and `include` are portable (Molto emits the right form per compiler);
`flags` is a raw escape hatch.

A relative `include` is resolved against the **project root**, not against the
directory Molto was invoked from. The manifest describes the project, so
`include = ["vendor"]` means the project's `vendor/` whether the build is run
from the root or from a subdirectory. Absolute paths are left alone, and
`flags` is untouched: it is passed verbatim by contract. These keys in `[target]` are the **base** applied
to every profile; a `[profile.*]` may declare the same keys, which are **added
on top** for that profile (see below).

Molto selects the correct driver per source file: the C one for `.c`, the C++
one for `.cpp`/`.cc`. Both come from the same resolved toolchain, so a project
mixing the two languages never mixes compilers. A project with C++ sources
needs a toolchain that has a C++ driver, which is part of what gets resolved:
a machine with `gcc-12` but no `g++-12` cannot build C++ with it, and that is
reported rather than discovered at link time.

A cross-compilation `triple` (and per-OS override tables such as
`[target.linux]`) are **reserved** for a future revision.

### Naming capabilities instead of compilers

A manifest states what a project needs; the compiler that provides it is a fact
about a machine, and machines differ. `compiler = "gcc"` means "the `gcc` on
this box", which on one system is version 9 and on another version 14.

So `compiler` no longer names a binary. It expresses a **preference of vendor**,
and it is optional. What a project actually depends on goes in `requires`:
features that must compile, each proven by compiling a program that uses it.

```toml
[target]
std      = "c2x"
requires = ["attr_nodiscard"]
```

That manifest builds on any machine that has *some* compiler able to do it, and
names none of them.

Resolution is performed by **pickup**, the toolchain manager (`spec.md` section
4). Molto invokes it, and records the answer in the workspace database so the
question is asked once rather than on every build (RFC-0004).

Accepting a `-std=` flag is not the same as implementing the standard behind
it: a compiler may take `-std=c2x` and reject `[[nodiscard]]`. `std` selects the
mode to compile in; `requires` states what has to work. A project that depends
on both should say both.

**Without pickup.** `C_COMPILER` and `CPP_COMPILER` name the drivers directly
and take precedence over resolution. They exist for a machine without pickup, a
compiler it does not detect, or to pin a build while investigating. Molto says
so on stderr when they are used, because such a compiler was chosen by hand and
never checked against `requires`.

## `[test]`

How `molto test` builds and lays out the test executables.

| Key       | Type          | Required | Description                                                                                     |
|-----------|---------------|----------|-------------------------------------------------------------------------------------------------|
| `mode`    | string        | no       | `per_file` (default) or `single`. See below.                                                    |
| `sources` | array[string] | no       | Extra sources compiled into the tests only. Directories are walked; plain files taken as given. |
| `defines` | array[string] | no       | Added to `[target]`/`[profile.*]` when compiling tests.                                          |
| `include` | array[string] | no       | Likewise, e.g. a framework's headers.                                                           |
| `flags`   | array[string] | no       | Likewise, verbatim.                                                                             |

**`per_file`** builds one executable per file under `tests/`, each linked with
the project's objects (minus the app's `main.c`). Every test file supplies its
own `main()`. This is the default and the original contract.

**`single`** links every test object, the extra `sources`, and the project's
objects into one executable at `build/<profile>/tests/<package>_tests`. This is
what a framework that registers its cases needs: the test files declare cases
and have no `main()`, and the framework supplies one.

`sources` is also how a framework living outside `src/` gets compiled at all:

```toml
[test]
mode    = "single"
sources = ["modules/moltest/src"]
include = ["modules/moltest/include"]
```

## `[env]`

A table of `KEY = "value"` pairs. Each variable is injected both into the
environment of the compiler/linker invocations and into the execution of the
program under `molto run` and `molto test`. Values are strings.

Variables are exported **in the child process**, after forking and before the
command is executed: Molto's own environment is never modified, so one
project's `[env]` cannot leak into anything else the process does. A
non-string value is a manifest error.

## `[profile.*]`

One table per build profile: `debug`, `release`, `bench`, or the user-defined
`custom` name (`spec.md` section 13). `release` enables compiler optimizations
by default.

| Key          | Type          | Description                                             |
|--------------|---------------|---------------------------------------------------------|
| `opt_level`  | integer       | Compiler optimization level                             |
| `debug_info` | bool          | Whether to emit debug symbols                           |
| `defines`    | array[string] | Extra defines for this profile, added to `[target]`     |
| `include`    | array[string] | Extra include directories for this profile              |
| `flags`      | array[string] | Extra raw flags for this profile                        |

A profile's `defines`/`include`/`flags` are **added on top** of the `[target]`
base for that profile (e.g. `-flto` only in `release`). Changing any compile
setting (opt level, std, defines, flags, …) triggers recompilation: Molto
records the exact command per object and rebuilds when it changes. The keys
`lto`, `strip`, `sanitizers` and `warnings_as_errors` are **reserved** for a
future revision.

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
