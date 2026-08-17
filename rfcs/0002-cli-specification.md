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
- Exit code `0` on success, non-zero on failure. Compiler and toolchain errors
  are surfaced before Molto's own diagnostics. They are captured and reframed
  rather than passed through untouched (RFC-0011); what a tool said is kept word
  for word, and a line Molto could not read is printed as the tool wrote it.

## Commands

### `molto new <name>`

Creates a new project directory named `<name>` containing a `Project.toml`
and the conventional `src/`, `tests/` layout described in RFC-0001
(Philosophy), plus a starter `src/main.c` so the project builds and runs
immediately.

The generated manifest declares `[target].std`. Left undeclared, the language
standard is whatever the local compiler defaults to, which varies by toolchain
and version — the project would compile differently on different machines,
against the determinism RFC-0001 promises. The remaining `[target]` keys are
written commented out, so the manifest documents what can be set without
setting it.

It also writes a `.gitignore` listing the two directories Molto owns,
`build/` and `.bin/`. Both are derived from the sources and safe to delete
(RFC-0004), so neither belongs in version control — and without this, the
first `git add -A` would commit a binary workspace database that changes on
every build. Molto writes the file but does not initialize a repository:
generating an inert text file is not the same as assuming a version control
system. Any of these files that already exists is left untouched.

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
- `--refresh-toolchain` resolves the compiler again instead of reusing the
  answer recorded in the workspace database. Also accepted by `run` and `test`.
- `--jobs <n>`, `-j <n>` compiles at most `n` translation units at once.
  Without it the build takes every core. It is a cap on this invocation and is
  never recorded: two builds differing only in `-j` produce the same objects.
  Also accepted by `run`, `test`, `lint` and `fmt` — all five run the same
  pool, and a cap only some of them obeyed would be a cap in name only.
- Writes `compile_commands.json` at the project root, describing every unit it
  compiled — including the ones it found up to date — so clangd, clang-tidy and
  anything else that parses this code reads the same flags the build used
  (RFC-0007). `molto test` writes one covering `tests/` too.

### `molto run [args]`

Builds (if needed) and executes the resulting binary artifact. Trailing
`args` after `--` are forwarded to the program.

### `molto test`

Discovers and runs tests from the conventional `tests/` directory. No test
file list is required; discovery follows the filesystem convention from
RFC-0001.

By default each test file becomes its own executable and supplies its own
`main()`. A project whose framework registers its cases and owns `main()` sets
`[test].mode = "single"` (RFC-0003), and the whole suite links into one
executable instead. Either way `molto test` runs what was built and reports
pass or fail per executable.

### `molto clean`

```
molto clean [--all]
```

Removes `build/`, the directory holding compiled output. With `--all`, also
removes `.bin/`, discarding the incremental state so the next build starts
from nothing.

Only directories Molto produced are removed; the manifest and the sources are
never touched. Running it on an already clean workspace succeeds: the point is
to end up without those directories, not to have found them.

### `molto bench`

Discovers and runs benchmarks from the conventional `bench/` directory,
using the `bench` build profile by default.

### `molto lint`

Runs compiler diagnostics and Molto's own static checks (e.g. naming
convention violations, section 17 of `spec.md`) without producing build
artifacts.

### `molto metadata`

Writes a **CycloneDX 1.6** bill of materials describing the package and every
package it links: the exact version, the origin with its revision resolved, the
checksum, and the licence each recipe declares (RFC-0009 `[about]`). To stdout,
or to `--output <path>`.

It resolves the graph rather than reading `Molto.lock`, because the lock records
versions, origins and checksums and deliberately not licences — that fact
already lives in each recipe, and a lock file that stored it twice could
contradict itself (RFC-0008). In a project that has been built, resolving
reaches no network.

Packages that only `[dev-deps]` reaches are **out** by default: a bill of
materials describes what is in the artifact, and a test framework is not.
`--include-dev` adds them, marked `excluded` — CycloneDX for "in the graph, not
in the artifact".

The document carries **no timestamp and no serial number**, though the schema
allows both. Two runs over one graph have to produce one file: a document that
differs every time cannot be diffed, cached, or compared between machines, which
is most of what one is kept for. It is the same rule that makes `Molto.lock`
sorted.

There is no `--profile`. A profile decides the defines and flags on a compile
line, and none of that changes which packages the graph contains.

### `molto add <dependency>[@<version>]`

Adds a dependency to the `[deps]` table of `Project.toml`, writing an **exact
version**: the one given after `@`, or the newest the registry offers when it is
omitted. Accepts the same dependency sources defined in RFC-0003 (registry, git,
path, archive, recipe). `--dev` adds it to `[dev-deps]` instead, for a
dependency that must not reach the package's binary (RFC-0008).

### `molto remove <dependency>`

Removes a dependency entry from `Project.toml`, from either table.

### `molto login`

Obtains a token from a registry and stores it in `~/.molto/credentials.toml`,
readable only by its owner. The password is typed at a terminal with echo
disabled, or bypassed entirely with `--token` for non-interactive use;
`--registry` selects the registry. See RFC-0010.

### `molto publish`

Publishes the current package to a configured registry (public or private,
see `spec.md` sections 15–16). Requires a stored credential from `molto login`.

### `molto update`

Asks the registry for newer releases of the declared dependencies, reports what
would change, and rewrites the exact versions in `Project.toml`. Since the
manifest names exact versions and never ranges (RFC-0008), an update is a
deliberate edit that lands in the diff rather than a re-resolution that happens
on its own.

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
| 5    | Command declared in the CLI but not implemented yet |

A command listed in `--help` but not yet implemented MUST exit with 5, never
with 1: a script has to be able to tell "this is not built yet" apart from
"the build failed".

### `molto run` and the program's exit code

`molto run` is the one command whose exit status is not Molto's own. Once the
build succeeds and the program starts, Molto is a transparent launcher:

- the program's exit code is propagated verbatim, including values that
  overlap the table above;
- a program killed by signal N reports 128 + N, following shell convention;
- the codes above only describe failures that happen **before** the program
  runs (an invalid manifest, a failed build, a bad profile name).

A caller that needs to distinguish Molto's failures from the program's should
run `molto build` first and then the executable directly.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0003: Project Manifest](0003-project-manifest.md)
- [RFC-0007: Build System](0007-build-system.md) — what `build`, `run` and `test` do
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — `add`, `remove` and `update`, and exit code 3
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — the document `publish` reads
- [RFC-0010: Registry Specification](0010-registry-specification.md) — `login` and `publish`
- [RFC-0011: Build Diagnostics](0011-build-diagnostics.md) — how what a compiler said reaches the person who typed the command
