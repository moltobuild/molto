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

### `molto new <name> [--lib | --bin]`

Creates a new project directory named `<name>` containing a `Project.toml`
and the conventional `src/`, `tests/`, `include/` layout described in RFC-0001
(Philosophy). What else it writes depends on what the project is:

- **A library, the default** (`--lib`): `artifact = "static"`, a header
  `include/<name>.h`, its source `src/<name>.c`, and a test
  `tests/test_<name>.c` written with moltest. The manifest declares moltest in
  `[dev-deps]` with `[test] mode = "single"`, so `molto test` works
  immediately.
- **A program** (`--bin`): `artifact = "executable"` and a starter
  `src/main.c`, so `molto run` works immediately.

Passing both flags is a usage error and writes nothing.

**Why a library by default.** Most C and C++ packages anyone depends on are
libraries, and a library is what a package manager distributes; a program is
the special case of a library with an entry point. The manifest's own default
for `artifact` stays `executable` (RFC-0003): changing it would silently turn
every existing manifest that omits the key into a library. The scaffold writes
`artifact` explicitly instead, so the choice is in the file and a reader learns
the key exists.

**moltest is offered, not required.** It is the ecosystem's tester and lives in
its own repository; the starter manifest names it like any other dependency,
and deleting that line and the test is all it takes to use something else.
Until moltest is published to the registry it is taken from git
(`https://github.com/moltobuild/moltest`, branch `master`).

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

### `molto init [--lib | --bin]`

Same as `new`, including the flags and the library default, but initializes a
project in the current directory instead of creating a new one.

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

`molto add git+<url>[#<ref>]` takes a git source in the spelling `Molto.lock`
already uses for one. The package name is the repository's: the last segment of
the URL, without `.git`; when that is not a valid package name, the command
says so and points at `molto add <name> --git <url>`. A git dependency needs a
branch, tag or rev before a build can cache it, so the reference is decided
here and written into the manifest, the way a registry's newest version is:
without `#<ref>`, the repository is asked for its default branch and that is
written as `branch`; with one, it is written as `rev` for a commit id and as
`tag` or `branch` for whichever the repository says it is (a tag when both).

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
`Project.toml` (see `spec.md` section 18).

It does **not** reorganize sources. An earlier draft of this section said it
would move files to follow Molto's conventions, and that contradicts the rule
this RFC applies to the manifest itself: Molto writes to a user's tree only in
response to an explicit command, and rearranging someone's repository is a far
larger act than writing one file they asked for. What `migrate` produces is a
manifest that describes the tree as it is, and a report of what it could not
express.

`migrate` is not a separate importer. It runs a compatibility frontend
(RFC-0014) once and serialises the resulting IR (RFC-0013) to a `Project.toml`.
Written any other way it would be a second parser for the files a frontend
already parses, and the two would drift — one parser, two products: a permanent
conversion here, and a continuous translation when the same project is built
through its own build files.

### `molto plugin <list|info|install|remove>`

Manages installed plugins (RFC-0014). `list` shows what is installed with the
capabilities and permissions of each; `info <name>` shows one in full, including
where it came from; `install <name>[@<version>]` fetches and verifies it from a
registry, shows what it will be allowed to do and asks for confirmation;
`remove <name>` uninstalls it.

Permissions are shown by default rather than behind a flag. The security of the
design rests on a user being able to answer "what is installed, and what may it
do" without reading a recipe by hand.

### `molto ir`

Writes the project's IR document (RFC-0013) to standard output, or to
`--output <path>`. It runs the frontend and every transform and stops before the
engine.

Like `molto metadata`, its output carries no timestamp, no serial and no path
that a second machine would write differently: two runs over one project MUST
produce one byte-identical file. A document that differs between runs cannot be
diffed, and a contract nobody can diff is a contract nobody can conform to.

### `molto <plugin-command>`

A plugin providing the `command` capability adds a subcommand under its own
name. The lookup happens **after** the built-in table, never before, so no
plugin can shadow `build`, and an unknown command that matches no plugin is
still a usage error.

## Exit Codes

| Code | Meaning                                   |
|------|--------------------------------------------|
| 0    | Success                                     |
| 1    | Build or compiler failure                   |
| 2    | Invalid or missing `Project.toml`           |
| 3    | Dependency resolution failure               |
| 4    | Invalid CLI usage (bad flags/args)          |
| 5    | Command declared in the CLI but not implemented yet |
| 6    | Plugin failure                              |

A command listed in `--help` but not yet implemented MUST exit with 5, never
with 1: a script has to be able to tell "this is not built yet" apart from
"the build failed".

6 is for a plugin that crashed, timed out, returned a document that failed
validation, or asked for something it had no permission to do (RFC-0014). It
exists for the same reason 5 does: a script needs to tell "my code does not
compile" apart from "a third-party binary misbehaved", and collapsing the two
into 1 makes a build failure the explanation for everything. The existing codes
still cover the neighbouring cases — a manifest naming an unknown plugin is 2,
and a plugin that cannot be resolved from a registry is 3.

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
- [RFC-0013: Build Intermediate Representation](0013-build-intermediate-representation.md) — the document `molto ir` writes
- [RFC-0014: Plugin System](0014-plugin-system.md) — `molto plugin`, plugin subcommands, the reframed `migrate`, and exit code 6
