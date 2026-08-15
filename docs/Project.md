# Configuring `Project.toml`

A practical guide to setting up a C or C++ project so `molto build`, `molto run`
and `molto test` work.

This document describes **what Molto implements today**.
[RFC-0003](../rfcs/0003-project-manifest.md) is the design specification: it also
covers tables that are reserved or not implemented yet. When the two disagree,
this file describes the binary you are running and the RFC describes the
intent.

## The one rule

**Molto discovers your sources; it does not discover your build settings.**

Every `.c`, `.cpp` and `.cc` file under `src/` and `tests/` is compiled
automatically — you never list files. But include paths, defines, link
libraries and how tests are laid out are only what the manifest says. Nothing is
inferred from the directory layout, and there is no fallback to a `Makefile`.

A manifest that omits `include = ["include"]` builds a project with no `-I`, and
every `#include <yourlib/foo.h>` fails. That is the single most common cause of
"it builds with make but not with molto".

## Location and discovery

One `Project.toml` at the project root. Commands search the current directory
and its ancestors, so `molto build` works from any subdirectory.

All relative paths in the manifest (`include`, `test.sources`) resolve against
the **project root**, never the directory you invoked Molto from. Absolute paths
are used as given. `flags` is never rewritten: it is passed verbatim by
contract.

## Starting point

`molto new my_app` creates `src/`, `tests/`, `include/`, a starter `main.c`, a
`.gitignore` and this manifest:

```toml
[package]
name = "my_app"
version = "0.1.0"
# description = ""      # one line, for a catalogue
# license = "MIT"       # an SPDX expression: MIT OR Apache-2.0
# homepage = ""
# repository = ""
# authors = []

[target]
std = "c17"            # C standard passed as -std=
include = ["include"]  # -Iinclude: the project's own headers
# compiler = "gcc"     # gcc | clang | msvc; absent = autodetect
# cpp_std = "c++17"    # C++ standard for C++ translation units
# link = ["m"]         # system libraries: -lm
# defines = []         # -D...
# flags = []           # passed verbatim to the compiler and the linker

[profile.debug]
opt_level = 0
debug_info = true

[profile.release]
opt_level = 3
debug_info = false
```

`std` and `include` ship active; the rest are commented documentation.

Adopting an existing project with `molto init` writes the same manifest, taking
the package name from the current directory's name. If that project keeps its
headers somewhere other than `include/`, that is the first line to change.

## Common tasks

### My headers live in `include/`

Already covered by the default manifest. A header at `include/greet.h` is
reachable as `#include <greet.h>`; one at `include/my_app/greet.h` as
`#include <my_app/greet.h>`.

For a different layout, name the directories you want on the include path:

```toml
[target]
include = ["include", "vendor/include"]
```

### I need a system library

```toml
[target]
link = ["m", "pthread"]     # -lm -lpthread
```

Names only, no `-l` prefix. For a flag the linker needs that is not a library
(`-pthread` as a compile flag, `-Wl,...`), use `flags`.

### I need defines and warnings

```toml
[target]
defines = ["_DEFAULT_SOURCE"]
flags   = ["-Wall", "-Wextra", "-Wpedantic"]
```

`defines` is portable — Molto emits the right form per compiler. `flags` is the
raw escape hatch, passed through untouched to both the compiler and the linker.

### I want my program to report its own version

You do not have to declare anything. Molto passes the package's name and version
to every translation unit it compiles, taken from `[package]`:

```c
#ifndef MOLTO_PKG_VERSION
#  define MOLTO_PKG_VERSION "0.0.0-unknown"   /* built by something else */
#endif

printf("%s %s\n", MOLTO_PKG_NAME, MOLTO_PKG_VERSION);
```

Both arrive as string literals. Writing the version down a second time in a
header is what this replaces: a binary answering with the version before last
looks exactly like one that was never rebuilt.

They are added on top of your own `defines` rather than counted against the
sixteen the manifest allows, and changing the version recompiles the project —
the version is part of the command line, and a stale object would carry the old
one.

### I want a specific language standard

```toml
[target]
std     = "c17"      # -std=c17, for .c files
cpp_std = "c++20"    # -std=c++20, for .cpp / .cc files
```

Molto picks the C driver for `.c` and the C++ driver for `.cpp`/`.cc`, both from
the same resolved toolchain, so a mixed project never mixes compilers. A project
with C++ sources needs a toolchain that has a C++ driver; if it does not, that
is reported rather than discovered at link time.

Omitting `std` inherits the compiler's default, which differs by toolchain and
version. Declare it.

### I need a compiler that really supports a feature

Accepting `-std=c2x` is not the same as implementing it: a compiler may take the
flag and still reject `[[nodiscard]]`. `std` selects the mode; `requires` states
what has to actually compile.

```toml
[target]
std      = "c2x"
requires = ["attr_nodiscard"]
```

Each requirement is **proven** by compiling a program that uses it. Resolution is
done by `pickup`, the toolchain manager, and the answer is recorded in the
workspace database (`.bin/`) so the question is asked once, not on every build.
`molto build --refresh-toolchain` asks again.

`compiler` is a *vendor preference* (`gcc`, `clang`/`llvm`, `msvc`), not a
binary, and it is optional. Naming a vendor instead of a version is what keeps a
manifest portable: `gcc` is version 9 on one machine and 14 on another.

Without pickup, `C_COMPILER` and `CPP_COMPILER` name the drivers directly and
take precedence. Molto says so on stderr when they are used, because a compiler
chosen by hand was never checked against `requires`. `MOLTO_PICKUP` points at an
uninstalled pickup binary.

### My tests use a framework with its own `main()`

This is the second most common cause of a project that builds but will not test.

By default (`mode = "per_file"`) Molto builds **one executable per file** under
`tests/`, each linked against the project's objects minus `src/main.c`. Every
test file supplies its own `main()`.

A framework that registers its cases owns `main()` instead, so everything has to
link into one binary. And if the framework lives outside `src/`, its sources are
not compiled at all unless you say so:

```toml
[test]
mode    = "single"
sources = ["modules/moltest/src"]        # directories are walked, files taken as given
include = ["modules/moltest/include"]
```

That produces `build/<profile>/tests/<package>_tests`. `[test]` also accepts
`defines` and `flags`, applied only when compiling tests.

### I want different settings per profile

```toml
[profile.release]
opt_level  = 3
debug_info = false
flags      = ["-flto"]      # added on top of [target], release only
```

Profiles: `debug` (default), `release`, `bench`, `custom`. Select with
`molto build --profile release`. Output goes to `build/<profile>/`.

`defines`/`include`/`flags` in a profile are **added on top of** the `[target]`
base, never replace it. Changing any compile setting triggers recompilation:
Molto records the exact command per object and rebuilds when it changes.

### I need environment variables during the build or the run

```toml
[env]
MY_APP_LOG = "debug"
```

Exported into the compiler and linker invocations, and into the program under
`molto run` and `molto test`. Variables are set **in the child process** after
forking, so Molto's own environment is never modified and one project's `[env]`
cannot leak elsewhere. Values must be strings.

Changing `[env]` recompiles and re-links, for the same reason changing a define
does: it is part of what an object was built from, not merely how it was
launched. The order the variables are written in is not — they are sorted by
name, so moving two lines rebuilds nothing.

## Key reference

Status is what the current binary does, not what RFC-0003 specifies.

### `[package]`

| Key | Type | Status | Notes |
|---|---|---|---|
| `name` | string | implemented | Required, `snake_case`, must start with a lowercase letter |
| `version` | string | implemented | Free-form string; defaults to `0.0.0` |
| `artifact` | string | **rejected** | Declaring it is a hard error — see below |
| `description` | string | implemented | One line saying what the package is |
| `license` | string | implemented | An SPDX expression: `MIT`, `MIT OR Apache-2.0`, `Apache-2.0 WITH LLVM-exception` |
| `homepage` | string | implemented | URL |
| `repository` | string | implemented | URL |
| `authors` | array | implemented | At most 8 entries |

The last five are optional, and none of them reaches a compile line. They are
what a catalogue search shows, and what a report has to read to name the licence
of everything a build links.

**`[package]` is the one table in this manifest that fails closed**: an unknown
key in it is an error naming the key, where `[target]` and `[profile.*]` would
drop it in silence. The reason is who reads these keys. A misspelling in
`[target]` costs a setting on your own machine and the build is there to notice;
a `licence` dropped in silence publishes a package that claims no licence at
all, to everyone who resolves it, and nothing about the build looks wrong.

`license` is checked for shape, not against the SPDX identifier list — a list
embedded in the binary is a list that expires. `MIT OR`, `(MIT` and
`MIT Apache-2.0` are refused; a misspelled but well-formed identifier is not.

### `[target]`

| Key | Type | Status | Notes |
|---|---|---|---|
| `compiler` | string | implemented | `gcc`, `g++`, `clang`, `llvm`, `msvc`. Anything else is an error. Absent = autodetect |
| `std` | string | implemented | `-std=` for C sources |
| `cpp_std` | string | implemented | `-std=` for C++ sources |
| `requires` | array | implemented | Features proven by compiling, e.g. `attr_nodiscard` |
| `link` | array | implemented | Library names without `-l` |
| `defines` | array | implemented | `-D`, portable across compilers |
| `include` | array | implemented | `-I`, relative to the project root |
| `flags` | array | implemented | Verbatim, compiler-specific |

### `[test]`

| Key | Type | Status | Notes |
|---|---|---|---|
| `mode` | string | implemented | `per_file` (default) or `single` |
| `sources` | array | implemented | Extra sources for tests only; directories walked |
| `defines`, `include`, `flags` | array | implemented | Applied only when compiling tests |

### `[profile.debug|release|bench|custom]`

| Key | Type | Status | Notes |
|---|---|---|---|
| `opt_level` | integer | implemented | `-O` |
| `debug_info` | bool | implemented | `-g` |
| `defines`, `include`, `flags` | array | implemented | Added on top of `[target]` |

Defaults if the table is absent: `debug` = `{0, true}`, `release` = `{3, false}`,
`bench` = `{3, false}`, `custom` = `{2, true}`.

### `[env]`

Keys are the variable names, values must be strings.

### Limits

Exceeding one is a manifest error, never a silent truncation — dropping a flag
would produce a green build that used different options than you asked for.

| | Limit |
|---|---|
| entries in any array (`defines`, `include`, `flags`, `requires`, `test.sources`) | 16 |
| length of one such entry | 95 characters |
| entries in `link` / length of one | 32 / 63 characters |
| entries in `[env]` | 32 |
| `[env]` name / value length | 63 / 255 characters |
| `description` / `homepage` / `repository` length | 255 characters |
| `license` length | 63 characters |
| entries in `authors` / length of one | 8 / 127 characters |

## Dependencies

Two tables, and the difference between them is what ends up in your binary.

```toml
[deps]
sqlite = "3.53.4"                       # from a registry, at exactly this version
http   = { path = "modules/http" }      # a directory you are working on

[dev-deps]
tinytest = { path = "modules/tinytest" } # only ever compiled into your tests
```

`[deps]` is linked into what you ship. `[dev-deps]` is for test frameworks,
benchmark harnesses and fixtures — anything that exists only while you are
working on the package.

The separation is enforced, not documented. A development dependency's include
directories are put on the command line that compiles `tests/` and on no other,
so a file under `src/` that includes one **fails to compile**:

```
src/lib.c:1:10: fatal error: 'tinytest.h' file not found
```

That is the intended behaviour and it happens on the first build, not at link
time and not in production. If you hit it, either the include belongs in a
test, or the dependency belongs in `[deps]`.

Three rules worth knowing:

- **Versions are exact.** `"3.53.4"` means that version. `^`, `~`, `>=` and the
  rest are not part of the format, and a manifest carrying one is refused. A
  range would let a release nobody has read enter your build without a diff
  ([RFC-0008](../rfcs/0008-dependency-resolution.md)).
- **One version per package.** The two tables share one graph, because a test
  binary links your `src/` objects together with your test objects — two
  versions of one library there are duplicate symbols. Declaring `png` at
  different versions in the two tables is an error, not two copies.
- **Development dependencies are not transitive.** Only yours are resolved.
  A library you depend on does not drag its test framework into your build.

`molto add` and `molto remove` edit these tables for you:

```sh
molto add sqlite                      # the newest release, written as an exact version
molto add sqlite@3.53.4               # that one
molto add tinytest --dev --path ../tt # into [dev-deps]
molto remove sqlite
```

Adding a name you already have is how you upgrade it — the same move as
`npm install`. Without `@<version>` Molto asks the registry what the newest
release is and **writes that number into the manifest**, so the choice is made
once, in a diff you can read, and never again behind your back.

They edit lines rather than rewriting the file, so your comments, alignment and
key order survive — and re-adding a name at a new version replaces it where it
already sits, keeping any comment beside it. Every edit is parsed before it is
written: one that would leave a manifest Molto cannot read is refused and your
file is untouched.

Resolving writes `Molto.lock`, which records the whole graph — including the
transitive packages your manifest never mentions — with a `scopes` array saying
which builds reach each one. It is generated; commit it, and edit
`Project.toml` instead.

## Not implemented yet

These are specified in RFC-0003 but do nothing in the current binary:

| Table / key | Behaviour today |
|---|---|
| `package.artifact` | **Hard error.** Every project links an executable; `static`/`shared` would need `ar`, `-shared` and `-fPIC`, so the key is refused rather than accepted and ignored |
| `molto update` | Exits with code 5, and stays that way: `molto add <name>` is the upgrade, the way `npm install` is |
| A dependency published as a prebuilt library | Refused with a message. Only `[artifacts] type = "source"` can be consumed |
| `[features]`, `[build-deps]`, `[workspace]` | Not read |
| `dep.recipe`, `artifact`, `optional`, `features`, `default_features` | Refused rather than ignored |
| `target.triple`, per-OS tables | Not read |
| `profile.lto`, `strip`, `sanitizers`, `warnings_as_errors` | Not read |

Unknown tables and unknown keys are **ignored without warning**. A typo in a key
name is silently dropped, so if a setting seems to have no effect, check its
spelling against the reference above.

That is the opposite of `format.json` and `linter.json`, which refuse anything
they do not understand ([`docs/Style.md`](Style.md#style-configuration-fails-closed)).
The asymmetry is deliberate rather than an oversight: the tables above are
specified by RFC-0003 and simply not implemented yet, so a manifest that
declares one is valid by design and rejecting it would refuse a correct file.
A style key, by contrast, is either expressible for the backend or it is not.
The cost is real — a typo costs you an afternoon — and the trade closes once
the schema stops growing.

Inside `[deps]` and `[dev-deps]` the trade is already closed: those two tables
**do** fail closed. An unknown key, two sources, a version range or a key that
is specified but not implemented is an error naming the dependency, because a
dependency read wrong is a dependency that silently is not there.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `fatal error: 'pkg/foo.h' file not found` | No include path declared | `include = ["include"]` in `[target]` |
| Builds, but tests fail to link (missing or duplicate `main`) | Test framework owns `main()`, or the files expect to be linked together | `mode = "single"` in `[test]` |
| Tests fail on the framework's own headers or symbols | Framework lives outside `src/` | `test.sources` + `test.include` |
| Warnings you expect are missing | `flags` not declared | Add `-Wall -Wextra` to `[target].flags` |
| A `#ifdef` never fires | `defines` not declared | Add it to `[target].defines` |
| `undefined reference` to `sqrt`, `pthread_create`, … | Library not linked | `link = ["m", "pthread"]` |
| `unknown compiler '…'` | `compiler` names a binary, not a vendor | Use `gcc`/`clang`/`msvc`, or drop the key |
| `artifact '…' is not supported yet` | `package.artifact` declared | Remove it |
| `fatal error: '…' file not found` for a dependency that is declared | It is in `[dev-deps]`, and the file including it is under `src/` | Move the dependency to `[deps]`, or the include into a test |
| `is a version range, and versions are exact` | `^`, `~` or `>=` in a version | Write the one version you mean |
| `is required twice and not as the same thing` | Two dependents disagree about a package | Change one of the two the message names |
| `brings no recipe.toml at the root of its source` | A `path`/`git` dependency does not describe itself | Add a `recipe.toml` with `[artifacts]` beside its sources |
| A key seems to do nothing | Typo, or the key is not implemented | Check the reference and the table above |
| `not inside a molto workspace` | No `Project.toml` in this directory or its ancestors | `molto init` |
| `invalid package name` from `molto init` | The directory name is not `snake_case` (`my-app`, `MyApp`) | Write the manifest by hand with a valid `name` |
| `package name is missing or not snake_case` | `[package].name` absent or malformed | Lowercase letter first, then letters, digits and `_` |

### Worked example: a manifest that was missing everything

Pickup's manifest declared only its name, version and `std`, while its
`Makefile` passed `-Iinclude -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic` and
built its tests as a single binary against a vendored framework. `molto build`
failed on the first `#include`, and `molto test` would not have linked even with
the headers found. Everything the Makefile knew had to be stated:

```toml
[target]
std      = "c2x"
requires = ["attr_nodiscard"]
defines  = ["_DEFAULT_SOURCE"]
include  = ["include"]
flags    = ["-Wall", "-Wextra", "-Wpedantic"]

[test]
mode    = "single"
sources = ["modules/moltest/src"]
include = ["modules/moltest/include"]
```

Migrating from a Makefile is mostly this: read `CFLAGS` and `LDFLAGS`, and put
each piece under the key that owns it.

## Related

- [`docs/Style.md`](Style.md) — `molto fmt` and `molto lint`, and the two files that configure them
- [RFC-0003: Project Manifest](../rfcs/0003-project-manifest.md) — the schema specification
- [RFC-0002: CLI Specification](../rfcs/0002-cli-specification.md) — the commands
- [RFC-0004: Workspace](../rfcs/0004-workspace-specification.md) — `build/` and `.bin/`
