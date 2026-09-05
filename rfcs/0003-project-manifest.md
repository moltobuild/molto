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
artifact = "static"        # executable | static | shared; default: executable

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

| Key          | Type          | Required | Description                                                                 |
|--------------|---------------|----------|-----------------------------------------------------------------------------|
| `name`       | string        | yes      | Package identifier, `snake_case`                                            |
| `version`    | string        | yes      | Semantic version                                                            |
| `artifact`   | string        | no       | `executable`, `static`, or `shared` (default `executable`)                   |
| `description`| string        | no       | One line saying what the package is                                         |
| `license`    | string        | no       | An SPDX licence expression, e.g. `MIT OR Apache-2.0`                        |
| `homepage`   | string        | no       | URL of the project's page                                                   |
| `repository` | string        | no       | URL of the source repository                                                |
| `authors`    | array[string] | no       | At most 8 entries                                                           |

**Current state:** `executable`, `static` and `shared` are all built. `source`
is still refused: it describes a package a registry serves as sources, which is
a recipe's business (RFC-0009) rather than something built here, and accepting
the key while building nothing would misreport what Molto did.

**The default changed, and the change is the point.** An earlier revision of
this table gave `artifact` a default of `static` — written before anything
built a library, when the value could not be acted on. Honouring it literally
the moment libraries arrived would have turned every project already written
into a library it never asked for, so the default is now `executable`, which is
what those manifests have always meant. `executable` is spelled as a value for
the same reason: a default nobody can name is a default nobody can restate.

What each kind produces, and the names it produces them under, is
[RFC-0007](0007-build-system.md#what-a-built-thing-is-called).

### The publishing metadata

The last five keys were reserved by earlier revisions of this RFC and are
specified here. None of them is required, none reaches a compile line, and each
is empty when it is not stated. They exist because a resolved dependency graph
that cannot name the licence of what it links is not a bill of materials.

`license` is checked for **shape** — identifiers joined by `AND`, `OR` and
`WITH`, parentheses balanced, `+` allowed as a suffix — and not against the SPDX
identifier list. Embedding that list would mean shipping a list that expires,
and refusing a licence published after the binary was built is worse than
accepting one that is misspelled. What the check does catch is what a typo looks
like: a dangling operator, an unclosed paren, two identifiers with nothing
joining them.

RFC-0009 specifies the same five keys for a recipe, under `[about]`, and
requires the two to agree. They are read by one reader — `manifest_read_about`,
parameterised by the name of the table — because two readers of one format
drift, and the copy that only ever sees registry answers has no local file
anyone can diff against.

**`[package]` fails closed.** An unknown key in it is an error naming the key,
which is how `[deps]`, `[dev-deps]`, `format.json` and `linter.json` behave and
*not* how the rest of this manifest behaves. The asymmetry is deliberate, and the
reason is who reads these keys: everywhere else, a misspelling costs a setting
that did not take effect on the machine that typed it, and the build is there to
notice. A `licence` dropped in silence publishes a package that claims no licence
at all, to everyone who ever resolves it, and nothing about the build looks
wrong.

**Not yet:** `molto publish` does not check that a manifest's `[package]` and its
recipe's `[about]` agree, although this RFC and RFC-0009 both require it. The
publish path reads a recipe alone today and can run without a project around it,
so the check needs its own design.

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

A cross-compilation `triple` is **reserved** for a future revision. The per-OS
override tables are specified below and are not yet built.

### Per-OS overrides: `[target.linux]`, `[target.macos]`, `[target.windows]`

**Decided, not implemented.** Written down here so the decision is not taken
twice.

`[target]` states what is true of every host. Beside it, a table named for an
operating system states what is true of that one:

```toml
[target]
std     = "c2x"
flags   = ["-Wall", "-Wextra", "-Wpedantic"]
defines = ["MOLTO"]

[target.windows]
link    = ["ws2_32", "bcrypt"]
defines = ["WIN32_LEAN_AND_MEAN"]

[target.macos]
flags   = ["-framework", "CoreFoundation"]
exclude = ["src/platform_linux.c"]
```

**Per OS and not per triple.** What differs between `x86_64-windows` and
`aarch64-windows` is almost never a flag, a define or a source file; what
differs is the operating system. Keying on the triple would mean writing the
same Windows section once per architecture, and a project that gained a third
would silently not get it.

#### The merge rules

Three rules, and the third is the one that took the longest to settle.

1. **Lists append.** `link`, `defines`, `include`, `flags` and `exclude` in a
   per-OS table are added to what `[target]` said, not put in its place. The
   per-platform case is additive in practice — `-lws2_32` only on Windows, a
   framework only on macOS, `_GNU_SOURCE` only on Linux — and replacement has a
   failure nobody sees: the day a flag is added to `[target]`, the platforms
   that overrode that key silently stop receiving it.

2. **Scalars replace.** `std`, `cpp_std` and `compiler` are one value, so a
   per-OS table naming one is naming *the* one.

3. **Nothing is removed.** There is no syntax for taking an entry out of what
   `[target]` said, and this is a decision rather than an omission.

   A flag that does not apply to one platform does not belong in `[target]`. It
   belongs in the tables of the platforms that want it. That is more verbose and
   it is unambiguous, and the ambiguity it avoids is real: every compact
   encoding of "remove" collides with the syntax of the thing being encoded. A
   leading `-` cannot mean "remove", because a leading `-` is what a flag *is* —
   `[profile.release].flags = ["-flto"]` already writes it that way, and
   `[target].flags` is documented above as `raw, verbatim`. Requiring flags to
   be written without their dash to free the character up would make
   `[target].flags` and `[profile.*].flags` two different languages in one file,
   and it would still not work: `-fno-plt`, `-Wno-unused` and every other
   negative flag would be indistinguishable from a request to remove the
   positive one.

   If a case appears that genuinely cannot be expressed by moving the entry into
   the platforms that want it, removal earns its own spelling then — and it must
   use a character no compiler flag can begin with, so that a flag is always
   written as itself.

#### What belongs in here, and what does not

The line is who knows the answer.

`-lws2_32`, `-framework CoreFoundation`, `-D_GNU_SOURCE`, `src/platform_win.c`:
a person decides these and only that person knows them. They belong in the
manifest.

`libgreet.1.dylib`, `-Wl,-install_name,...`, `.exe`: these are derived from the
package name, its version and the platform, and molto composes them (RFC-0018,
RFC-0007). Nobody should be typing them, a manifest that could would be a
manifest that can declare a soname contradicting its own version, and the
mechanism that let it would also let a manifest remove `-fPIC` or `-o`.

A useful test: **if molto can derive it from the manifest and the platform, it
does not go in the manifest.**

#### `exclude` is the one that is missing most

`source_discovery` takes every `.c` under `src/` and there is no way to say
otherwise. A `src/platform_win.c` that includes `<windows.h>` therefore breaks
the Linux build, and the only way out is to wrap the whole file in an `#ifdef`.

Molto does this to itself: its 45 `_WIN32` sites live inside 8 shared files
rather than in per-platform ones, and that is the workaround rather than a
preference. Per-OS source selection is what would let a C project be laid out
the way C projects are laid out.

#### Where the merge happens, and what it must not reach

At manifest read, in `project_ctx`, before a document exists. The IR carries the
result and never the operation: a subtraction node in a document is something
RFC-0013 has no place for, and a document that can remove what another part of
it added is a document whose command line cannot be read off it.

A document keyed on the **target** is fine — molto already writes to
`build/<platform>/<profile>`, so a document has always described a build for a
platform. What must never reach it is the **host's** answer to a question about
the target, which is the mistake `artifact.path` refuses for `.exe`.

#### Non-goals

**No `[target.<os>.deps]`.** RFC-0017's strictest bar is that `Molto.lock` for
one project is byte-identical on every platform, and dependencies that vary by
platform break it. Flags, defines, includes, links and excludes do not reach
resolution, so they are safe; a dependency list is not.

#### Until it exists

An unknown table is ignored in silence today, so a manifest writing
`[target.macos]` right now gets nothing and is told nothing. That is the same
fail-open the `[plugins]` note in *Reserved Sections* describes, and it has the
same answer: this cannot ship before the manifest can declare a schema.

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

Because they reach the compiler and the linker, the variables are part of the
compile and link fingerprints (RFC-0007): changing one recompiles and re-links,
the way changing a define does. The order they are declared in is not part of
anything — they are sorted by name — so moving two lines rebuilds nothing, and
two projects declaring the same variables in different orders still share an
object in the cache.

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
| `version`          | string        | One exact semver version, never a range; fetched from a registry (source)      |
| `git`              | string        | Git repository URL (source); pair with one of `branch`/`tag`/`rev`             |
| `branch`           | string        | Git branch (with `git`)                                                        |
| `tag`              | string        | Git tag (with `git`)                                                           |
| `rev`              | string        | Git commit revision (with `git`)                                               |
| `path`             | string        | Local directory (source)                                                       |
| `archive`          | string        | Archive URL (source)                                                           |
| `registry`         | string        | Named registry to use (default: the official one)                              |
| `artifact`         | string        | How the dependency is built: `source`/`static`/`shared`                        |
| `optional`         | bool          | If true, only resolved when a feature enables it (default false)               |
| `features`         | array[string] | Features to enable in the dependency                                           |
| `default_features` | bool          | Whether to enable the dependency's default features (default true)            |

Rules:

- Exactly one source among `version`, `git`, `path`, `archive`. The
  plain-string form `dep = "1.2.3"` is equivalent to `{ version = "1.2.3" }`.
- There was a fifth, `recipe`, naming a recipe for a registry to resolve. It is
  **withdrawn**, and the reason is that `version` turned out to be it. A source
  recipe (RFC-0009) is published under a coordinate and resolved by version like
  anything else, which is how `zlib` and `yyjson` are consumed — neither was
  designed to be a Molto package, which is the case `recipe` was written for.
  Keeping it would have meant a second spelling of one thing, and the form
  RFC-0008 gave it (`recipe+<url>#<name>`) named no version at all, which is a
  floating dependency in a format whose central rule is that versions are exact.
- **A version is exact.** `^`, `~`, `>=`, `>`, `<`, `<=`, `*` and
  comma-separated conjunctions are not part of this format, and a value carrying
  one is a manifest error. RFC-0008 gives the reason: a range authorises code
  that does not exist yet to enter a build without a diff.
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
- `[build-deps]` — dependencies needed to run a build rather than to link into
  it. `[dev-deps]`, once reserved here alongside it, is specified in RFC-0008:
  same syntax as `[deps]`, resolved only for the root package, and never linked
  into the package's own binary.
- `[workspace]` — multi-package workspaces (`members`).
- A manifest schema/version key for forward compatibility.
- `[plugins]` — the plugins a project uses, specified by RFC-0014. It is
  reserved separately from `[build-deps]` although the two are close enough to
  be confused. A `[build-deps]` entry is a package: resolved by the resolver,
  recorded in the lock file, built from source. A `[plugins]` entry is an
  installed executable with declared permissions that Molto runs. They may share
  a resolver one day; they do not share a consent model, and folding "this
  binary may spawn processes" into a dependency list nobody reads that carefully
  is how a supply chain gets one.

`[plugins]` is also what forces the schema key above to stop being reserved. It
**MUST** fail closed, and it cannot: an unknown table is ignored in silence
today, so a manifest naming a plugin would build without it, succeed, and say
nothing — a green build of the wrong thing, which is the exact failure
`[package]`'s closed key list was introduced to prevent. An old reader cannot
know it should refuse a table it has never heard of, so the manifest has to tell
it: a manifest that names plugins declares a schema, and a Molto that does not
understand that schema refuses the file rather than reading the half it
recognises.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md)
- [RFC-0007: Build System](0007-build-system.md) — what `[target]`, `[test]` and `[profile.*]` produce on a command line
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — the algorithm behind `[deps]` and `[registries]`
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — reuses the `[deps]` syntax, and spells the publishing metadata `[about]`
- [RFC-0010: Registry Specification](0010-registry-specification.md) — how a named registry is reached
- [RFC-0014: Plugin System](0014-plugin-system.md) — `[plugins]`, and the schema key it forces
