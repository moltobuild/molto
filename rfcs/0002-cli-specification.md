# RFC 0002: CLI Specification

- RFC Number: 0002
- Title: CLI Specification
- Status: Draft
- Created: 2026-07-26

## Summary

This RFC specifies the `molto` command-line interface: its subcommands,
global conventions, and behavior. It expands on the component list defined
in [RFC-0001](0001-manifesto.md).

## Motivation

`molto` is the main entry point of the ecosystem. A precise CLI
specification is required so that:

- users have a predictable, Cargo-like experience
- alternative or partial implementations stay compatible
- documentation and shell completions can be generated from a single source
  of truth

## Global Conventions

- Invocation: `molto <command> [args] [flags]`
- `-h` / `--help` and `-V` / `--version` are available on every command.
- Flags are `--kebab-case`; identifiers inside generated code are
  `snake_case` (see RFC-0001, Philosophy).
- Commands that operate on a project require a `Project.toml` to be
  discoverable in the current directory or an ancestor directory (see
  RFC-0003).
- Exit code `0` on success, non-zero on failure. Compiler/toolchain errors
  are surfaced verbatim before Molto's own diagnostics.

## Commands

### `molto new <name>`

Creates a new project directory named `<name>` containing a `Project.toml`
and the conventional `src/`, `tests/` layout described in RFC-0001
(Philosophy).

### `molto init`

Same as `new`, but initializes a project in the current directory instead of
creating a new one.

### `molto build`

Compiles the project.

- `--profile <debug|release|bench|custom>` selects the build profile
  (default: `debug`). See RFC-0003, Build Profiles.
- Performs incremental compilation: only translation units whose source
  hash, dependency graph, or compiler flags changed are rebuilt (tracked via
  WSDB, see `spec.md` section 11).

### `molto run [args]`

Builds (if needed) and executes the resulting binary artifact. Trailing
`args` after `--` are forwarded to the program.

### `molto test`

Discovers and runs tests from the conventional `tests/` directory. No test
file list is required; discovery follows the filesystem convention from
RFC-0001.

### `molto bench`

Discovers and runs benchmarks from the conventional `bench/` directory,
using the `bench` build profile by default.

### `molto lint`

Runs compiler diagnostics and Molto's own static checks (e.g. naming
convention violations, section 17 of `spec.md`) without producing build
artifacts.

### `molto add <dependency>`

Adds a dependency to the `[deps]` table of `Project.toml`. Accepts the same
dependency sources defined in RFC-0003 (registry, git, path, archive,
recipe).

### `molto remove <dependency>`

Removes a dependency entry from `Project.toml`.

### `molto publish`

Publishes the current package to a configured registry (public or private,
see `spec.md` sections 15–16).

### `molto update`

Re-resolves dependency versions against the registry within the constraints
declared in `Project.toml`.

### `molto migrate <make|cmake|meson>`

Imports an existing project built with Make, CMake, or Meson, generating a
`Project.toml` and reorganizing sources to follow Molto's conventions where
possible (see `spec.md` section 18).

## Exit Codes

| Code | Meaning                                   |
|------|--------------------------------------------|
| 0    | Success                                     |
| 1    | Build or compiler failure                   |
| 2    | Invalid or missing `Project.toml`           |
| 3    | Dependency resolution failure               |
| 4    | Invalid CLI usage (bad flags/args)          |

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0003: Project Manifest](0003-project-manifest.md)
