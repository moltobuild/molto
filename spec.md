# Molto Specification

Version: 0.1.0-draft

Status: Draft

---

# 1. Vision

Molto is a modern packaging ecosystem for C and C++.

Its purpose is not to replace GCC, Clang, or MSVC, but to modernize the developer experience around them.

Molto provides:

- Project management
- Dependency management
- Reproducible builds
- Incremental compilation
- Package registry
- Plugin ecosystem
- Workspace metadata
- Project templates

while remaining compiler agnostic.

---

# 2. Goals

Molto should make C and C++ development feel similar to Rust's Cargo while preserving the flexibility and philosophy of native toolchains.

The project prioritizes:

- Convention over configuration
- Fast builds
- Zero configuration for common projects
- Cross-platform support
- Excellent compiler diagnostics
- Reproducible builds
- Extensible architecture

---

# 3. Non Goals

Molto is NOT:

- a compiler
- a linker
- a build language
- a replacement for GCC
- a replacement for Clang
- a replacement for MSVC

Molto orchestrates existing toolchains.

---

# 4. Components

The ecosystem consists of several independent components.

## molto

Main CLI.

Responsibilities:

- new
- init
- build
- run
- test
- bench
- lint
- add
- remove
- publish
- update
- migrate

---

## pickup

Toolchain manager.

Responsibilities:

- Detect installed compilers
- Install supported toolchains
- Manage compiler versions
- Configure global toolchains

Inspired by rustup.

Molto does not choose compilers. A manifest states the capabilities a project
needs and pickup answers which local binary provides them, so the manifest
never names a machine. Molto records that answer in the WSDB and asks again
only when the requirements change or the compiler is replaced.

---

## Registry

Stores:

- packages
- recipes
- metadata
- versions

Supports:

- public registries
- private registries

---

## Workspace

Stores workspace metadata.

Uses WSDB.

---

# 5. Project Manifest

Every project contains:

Project.toml

This file is the single source of truth.

---

# 6. Philosophy

Configuration should be optional.

The filesystem should describe the project whenever possible.

Example:

src/

controllers/

repository/

models/

tests/

bench/

No file lists are required.

Molto discovers source files automatically.

---

# 7. Dependency Model

Dependencies may come from:

- Registry
- Git
- Local path
- Archive
- Recipe

Example

```toml
[deps]

yyjson = "1.2.32"

sqlite = {
    git = "https://github.com/sqlite/sqlite",
    tag = "3.50.0"
}

http = {
    path = "modules/http"
}
```

---

# 8. Recipes

A Recipe describes how a dependency is obtained and built.

A Recipe may point to:

- Git repository
- Archive
- Local directory

Recipes are stored in registries.

---

# 9. Artifacts

Molto supports three artifact types.

- source
- static
- shared

Default:

static

Artifacts are cached globally.

Source repositories are never cached by Molto.

---

# 10. Global Cache

Global cache stores only reusable build artifacts.

Example

~/.molto/

cache/

include/

lib/

No source repositories are stored.

---

# 11. Workspace Database

Every workspace contains

.bin/

Inside:

WSDB

WSDB is a binary database describing the workspace.

It stores:

- dependency graph
- build graph
- file hashes
- timestamps
- compiler metadata

WSDB is never edited directly.

Only the Molto API may access it.

---

# 12. Incremental Compilation

Only modified translation units are rebuilt.

Molto tracks:

- file hashes
- dependency graph
- compiler flags

Every build also writes `compile_commands.json` at the project root, describing
each translation unit — including the ones it found up to date. Editors and
static analysers read the flags the build used instead of guessing them
(RFC-0007).

---

# 13. Build Profiles

Profiles include:

debug

release

bench

custom

Release enables compiler optimizations automatically.

---

# 14. Plugins

Plugins extend Molto.

Examples

molto lambda

molto deb

molto rpm

molto appimage

Plugins communicate using a stable API.

---

# 15. Registry Philosophy

Registries store metadata.

They may optionally store packages.

A registry may reference external Git repositories.

Official maintainers may publish packages directly.

---

# 16. Private Registries

Organizations may implement private registries.

The protocol is public.

Compatible implementations may use any backend.

Examples:

- PostgreSQL
- MySQL
- SQLite
- D1

Storage:

- R2
- S3
- Azure Blob
- MinIO

---

# 17. Coding Conventions

Molto promotes:

snake_case

Example

```c
#include <controllers/user_controller>
```

instead of

```c
#include "../controllers/UserController.h"
```

Automatic migration tools are provided.

---

# 18. Migration

Molto can import projects from:

- Make
- CMake
- Meson

Command

molto migrate

---

# 19. Cross Platform

Supported platforms:

Linux

Windows

macOS

Supported compilers:

GCC

Clang

MSVC

---

# 20. Performance

Performance is a core feature.

The ecosystem provides:

bench/

performance reports

incremental builds

parallel compilation, on every core by default and capped with `-j`

artifact caching

compiler optimization presets

---

# 21. Design Principles

Every feature must satisfy:

- Simple by default
- Explicit when needed
- Fast
- Portable
- Deterministic
- Backwards compatible whenever possible

---

# 22. RFC Process

All major features are specified through RFCs.

RFCs are immutable once accepted.

New behavior must be introduced through new RFCs.

---

# 23. Roadmap

Version 0.1 — shipped

- Project creation
- Build
- Run
- Test
- GCC support
- Linux

Version 0.2 — shipped

- Format and lint, with a canonical style model (RFC-0005)
- Toolchain resolution delegated to pickup, driven by declared requirements
- Incremental and parallel builds, specified after the fact (RFC-0007)
- Self-hosting: molto builds molto

Version 0.3

- Clang
- Registry (RFC-0010)
- Recipes (RFC-0009)
- Dependency resolution (RFC-0008)

Version 0.4

- Windows
- macOS
- Plugins

Version 1.0

Stable API
Stable Registry
Stable Plugin ABI

The 0.1 and 0.2 entries record what was released. Clang, Windows and macOS were
planned for 0.2 and did not make it: the work went into the toolchain contract
with pickup and into style instead. They are not dropped, only moved.
