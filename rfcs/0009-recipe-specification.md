# RFC 0009: Recipe Specification

- RFC Number: 0009
- Title: Recipe Specification
- Status: Draft
- Created: 2026-08-07

## Summary

This RFC defines the **Recipe**: the TOML document that describes an artifact
the ecosystem can distribute. It specifies the coordinate every recipe carries,
the `[source]`, `[build]` and `[artifacts]` tables that describe how something
is obtained, built and consumed, the tables specific to each kind, and the
compatibility rules that let a recipe written today be read by a Molto released
later.

It formalises `spec.md` section 8, gives RFC-0008 the shape of what a `version`
dependency resolves to when the package is not one, and writes down the contract
a registry already validates (RFC-0010).

## Motivation

There are two documents called a recipe and they are not the same thing.

`spec.md` §8 says a recipe "describes how a dependency is obtained and built"
and "may point to a Git repository, an Archive, or a Local directory". That is a
build description: a thing you hand to Molto and it produces a library.

The recipe that exists describes nothing of the sort. It is a coordinate — kind,
name, version, target — plus a declaration of what the artifact offers, and it
accompanies a `tar.zst` that somebody built by hand and uploaded. There is no
`[source]`, no `[build]`, and nothing in the ecosystem could act on them if
there were. `molto publish` reads four keys out of it and posts the rest
verbatim.

Both documents are needed and neither is wrong. Toolchains are the clearest
case: nobody is going to build GCC on the machine that wants to use it, so the
recipe for a toolchain describes a binary that already exists. `libpng` is the
opposite case: it has a `configure` script, it predates every package manager
that would want it, and the only way to consume it is to describe how to build
it. The mistake would be to pick one and pretend the other does not exist.

## Two kinds of recipe

This RFC therefore specifies **one format with two modes**, distinguished by an
explicit key rather than by which tables happen to be absent:

- A **binary recipe** (`form = "binary"`) describes an artifact that has already
  been built. The bytes live in the registry; the recipe says what they are and
  what a consumer gets from them. This is what exists today, and it is the only
  form a `toolchain` or a `tool` may take.
- A **source recipe** (`form = "source"`) describes how to obtain and build
  something that is not a Molto package. It carries `[source]` and `[build]`,
  and the registry stores the recipe alone — there are no bytes to upload. This
  is what a `version` dependency resolves to when the thing on the other end
  was never a Molto package. RFC-0008 once had a `recipe` source for naming one
  of these directly; it is withdrawn, and that document says why.

The discriminator is explicit because inference is what turns a typo into a
different document. A source recipe with a misspelled `[souce]` table would, by
inference, be a valid binary recipe with a missing archive; declared, it is an
error at publish time.

A Molto package needs neither form. Its `Project.toml` already says how to build
it and Molto already knows how (RFC-0007), so it is published as a package and
resolved by version. A recipe is for everything that is not one.

## Coordinates

Every recipe, in both forms, opens with the coordinate that identifies it:

| Key | Type | Description |
|---|---|---|
| `schema` | integer | Recipe format version (see Compatibility) |
| `form` | string | `binary` or `source` |
| `kind` | string | `toolchain`, `tool` or `package` |
| `name` | string | Matches `^[a-z0-9][a-z0-9._-]*$` |
| `version` | string | Matches `^[A-Za-z0-9][A-Za-z0-9.+-]*$` |
| `target` | string | Matches `^[a-z0-9][a-z0-9_-]*$`, or `any` |

`(kind, name, version, target)` is the primary key of the whole ecosystem. It is
what a registry stores, what a storage key is derived from, and what pickup and
Molto both address — one protocol for compilers, tools and libraries, because
they differ in what they contain and not in how they are named.

`target` is a platform triple, or the literal `any` for something that does not
depend on one: a header-only library, or a source recipe whose build happens on
the consumer's machine. The version is deliberately *not* required to be semver
here — a toolchain called `13.2.0-x86_64` is a real thing — but a `package`
must be, because RFC-0008 has to order versions to propose the newest one.

**Coordinates are immutable.** A published coordinate is never overwritten and
never means different bytes later (RFC-0010). Republishing is a new version.

## `[source]`

Source recipes only. Where the bytes come from, and how to know they are the
right ones. Exactly one origin key, mirroring the dependency sources of
RFC-0003 so a reader learns one vocabulary:

| Key | Type | Description |
|---|---|---|
| `git` | string | Repository URL; pair with `tag` or `rev` |
| `tag` / `rev` | string | The commit to check out; a `tag` is resolved to a `rev` |
| `archive` | string | Archive URL |
| `sha256` | string | Required with `archive`; the digest of the archive |
| `path` | string | A local directory, for developing a recipe |
| `strip_prefix` | string | Leading directory to drop when unpacking an archive |
| `compression_format` | string | How the archive is packed; absent means infer |

An `archive` without a `sha256` is invalid. A URL is a promise about a location,
not about content, and a recipe whose output changes when upstream re-rolls a
tarball is not a recipe — it is a suggestion. `git` needs no digest because a
commit id is one.

`compression_format` is one of `zip`, `tar`, `tar.gz`, `tar.bz2`, `tar.xz` or
`tar.zst`, and a name outside that list is a rejected recipe rather than a
fallback to guessing. It exists because an extension is a naming convention and
not a fact: a URL may serve a tarball from a path ending in `/download`, or end
in `.zip` and serve something else. Absent, a reader infers as it always did —
`.zip` means zip and everything else is handed to `tar`, which detects gzip,
bzip2, xz and zstd from the bytes — so the key could be added without a flag
day and an older recipe keeps working.

It belongs to `archive` alone. A `git` origin is a checkout and a `path` is a
directory; neither is packed, so declaring it beside one is an error rather
than a key quietly ignored — a recipe that sets it believes something about
what the consumer will do.

Note what this key is **not**: a hook. The temptation is to let a recipe carry
a shell command that unpacks it, and that is the same temptation `[build]`
refuses for the same reason. A recipe that could run a command would make every
dependency a remote code execution with extra steps, and the format that
results from naming a bounded set of packings is both safer and easier to read.

## `[build]`

Source recipes only. How the thing is built — by naming a build system, never by
carrying a script.

| Key | Type | Description |
|---|---|---|
| `system` | string | `make`, `cmake`, `autotools`, `meson` or `none` |
| `via` | string | `frontend` or `delegate`; how `system` is honoured |
| `args` | array[string] | Arguments passed to it, verbatim — `delegate` only |
| `env` | table | Environment variables set for the build — `delegate` only |
| `jobs` | bool | Whether the build system is given a parallelism flag — `delegate` only |

`system = "none"` is for a source drop that needs no build: headers, or sources
compiled by the consumer as if they were its own.

This table is where the temptation to invent a build language lives, and the
answer is no (RFC-0001, and RFC-0007's Non-Goals). A recipe that could run
arbitrary commands would make every dependency a remote code execution with
extra steps, and would make Molto the thing it says it is not. Naming an
existing build system keeps the recipes people write readable — `system =
"cmake"` says more about a dependency than twenty lines of shell.

A build system Molto does not know is a rejected recipe, not a fallback to
`sh -c`.

### `system` names a language; `via` names the mechanism

This document used to argue that naming an existing build system avoided
arbitrary code execution, because "the escape hatch is `args`, which is
bounded". That argument was wrong and is withdrawn. `system = "cmake"` runs
CMake, and a `CMakeLists.txt` may call `execute_process`; `system = "meson"`
runs Meson, and a `meson.build` may call `run_command`. Handing a build to an
installed build system *is* remote code execution with extra steps — the thing
this table was written to prevent — and until RFC-0014 it was the only option a
recipe had.

So `system` names the **language the build is described in**, and `via` names
**what Molto does with it**:

- `via = "frontend"` — a compatibility plugin (RFC-0014) reads the build files
  and translates them into an IR document (RFC-0013). Nothing from the
  dependency is executed. A construct outside the frontend's subset is a
  rejected build, reported with its line, and **never** a silent fallback to
  `delegate` — a safe path that becomes unsafe exactly when a project gets
  complicated is not a safe path.
- `via = "delegate"` — the build system installed on this machine is invoked,
  as it always was. This is the escape hatch, and calling it one in the recipe
  is the point: it is a declaration, readable from the catalogue before a single
  byte is downloaded (RFC-0010), that building this dependency runs code the
  dependency wrote.

`via` defaults to `frontend` when a frontend for that `system` is installed, and
to `delegate` otherwise, so a recipe written today keeps working and gets safer
the day a frontend exists for it. A recipe may set `via` explicitly to refuse
that: a dependency that genuinely needs its own configure logic pins
`delegate`, and one that never wants to run foreign code pins `frontend` and
fails loudly if none is installed.

`args`, `env` and `jobs` mean nothing under a frontend. There is no process to
pass arguments to, no environment to set for it, and — since Molto schedules the
work itself once it is in the graph (RFC-0015) — nothing to hand a parallelism
flag to. Setting any of the three alongside `via = "frontend"` is a rejected
recipe, not a key quietly ignored: a recipe that sets them believes something
about what the consumer will do.

## `[artifacts]`

What the consumer gets. This is the join with RFC-0007: everything declared here
ends up on a compile or link line of the project that depends on it.

| Key | Type | Description |
|---|---|---|
| `type` | string | `source`, `static` or `shared` (default `static`) |
| `std` | string | The C standard its own sources compile with; absent means the consumer's |
| `cpp_std` | string | The same for C++, decided independently of `std` |
| `sources` | array[string] | The sources a consumer compiles; absent means all of them |
| `exclude` | array[string] | Sources to drop, applied after `sources` |
| `include` | array[string] | Directories added as `-I`, relative to the artifact root |
| `link` | array[string] | Libraries added as `-l` |
| `defines` | array[string] | Defines added as `-D` |
| `flags` | array[string] | Raw flags a consumer must compile with |

Everything in that table is the package's **interface**: it reaches the compile
and link lines of whatever depends on it. `[artifacts.private]` is the other
half, and carries the same three option keys:

| Key | Type | Description |
|---|---|---|
| `include` | array[string] | Directories added as `-I`, for this package only |
| `defines` | array[string] | Defines added as `-D`, for this package only |
| `flags` | array[string] | Raw flags, for this package only |

They apply when this package's own sources are compiled and nowhere else. The
split exists because the two are different claims and were being made with one
word. `SQLITE_THREADSAFE=1` is interface: it changes what the header declares,
so a consumer that does not set it is compiling against a different library.
`-fno-strict-aliasing`, `-Wno-implicit-fallthrough` and an `-I` into the
package's own internals are not: they are how this code happens to build, and a
caller that inherits them has inherited a mistake. Without somewhere to put the
second kind, every one of them is everybody's — including the sources of
unrelated dependencies that were never told about this package at all.

`std` and `cpp_std` are what a package says about the language it was written
in. Without them a library written against C99 is compiled as C23 by whoever
asked for C23, and its author has no way to say otherwise — the one thing a
package could not decide about itself once flags were scoped. They are declared
per language and inherited per language: a C library with a single C++ shim
names `std` and lets `cpp_std` be the consumer's.

Unlike a define, a standard does **not** travel to whoever depends on the
package. A define can change what a header declares, so a caller that does not
share it is compiling against a different type; a standard is about how a
source is parsed, and every translation unit is parsed on its own.

Both values are **checked against the standards Molto knows**, which
`[target].std` in a manifest is not, and the asymmetry is the point: a
misspelled standard in a manifest fails in the build of the person who typed
it, while a misspelled one in a published recipe fails in the build of everyone
who depends on them, under a compiler option none of them wrote. Each key is
checked against its own language, so a `c++20` written under `std` is caught
too — a real standard, and still the wrong answer to that key.

One known limit: the standard a recipe names does not take part in resolving
the toolchain. A recipe asking for a standard the resolved compiler does not
implement fails when its sources are compiled, not when the compiler is chosen.

There is no private `link`, because a `-l` names a library the final binary is
linked against and there is no line it could be private to. There is no private
`std` either, for the opposite reason: a standard never leaves the sources it
applies to, so there would be nothing for a private one to distinguish itself
from. There is no private
`sources` or `exclude` either: a package has one set of sources, and `sources`
and `exclude` already cut it from both directions (`sources` fails closed,
`exclude` fails open, and `exclude` is applied second so a list can be narrowed
rather than restated).

A recipe that declares only `[artifacts.private]` is read like any other. It is
a real shape — a package whose one statement is a warning it silences — and
skipping it because nothing was written directly under `[artifacts]` would drop
the only thing the recipe was written to say.

`sources` and `exclude` exist because a source drop is not the same thing as a
library. An upstream archive contains what upstream ships, and what upstream
ships is usually more than the library: SQLite's amalgamation carries `shell.c`,
which has its own `main()`, so a consumer that compiles the whole drop links two
of them and fails on a duplicate symbol. There is no rule that could infer this
— `main` is a legitimate symbol in a source file — so the recipe has to say.

They are two keys rather than one because they fail in opposite directions.
`sources` fails closed: a file added upstream tomorrow does not join the build
by itself, which is what a dependency pinned to a version should mean. `exclude`
fails open, and is the shorter statement when a drop is almost entirely library
and the exception is named. Naming both is allowed and `exclude` wins, so a
recipe can narrow a list it inherits without rewriting it.

`flags` is the escape hatch and, as in RFC-0003, it is passed verbatim and is
the least portable thing a recipe can contain. A recipe that needs it for
anything other than a genuine ABI requirement — `-pthread`, say — is usually
describing something that belongs in `defines`, or in
`[artifacts.private].flags` when it is about how this package builds rather
than about how it is used.

`type = "source"` means the consumer compiles the dependency's sources into its
own build rather than linking a prebuilt library. It is the only type that
travels between platforms unchanged, and it is why `target = "any"` and
`type = "source"` usually appear together.

## Kind-specific tables

### `[toolchain]`

Required for `kind = "toolchain"`. `vendor`, `triple` and `c_driver` are
mandatory strings. Optional `[toolchain.c]` and `[toolchain.cxx]` tables each
carry the string lists `std`, `compile_flags`, `link_flags` and `runtime_dirs`.
A top-level `provides` string list declares the capabilities the toolchain
satisfies — the same vocabulary a manifest's `[target].requires` asks for
(RFC-0003), which is what lets pickup answer "which local compiler provides
`attr_nodiscard`" without Molto naming a binary.

### `[tool]`

Required for `kind = "tool"`. `kind` is one of `formatter`, `linter`, `linker`
or `plugin`, and `binary` names the executable inside the archive. An optional
`aliases` string list gives the other names the tool is known by. This is what
lets `molto fmt` and `molto lint` obtain their tools from the same registry
that serves libraries (RFC-0005).

`kind = "plugin"` additionally requires a `[plugin]` table, specified by
RFC-0014, declaring the capabilities the plugin provides, the file extensions
that select it, the permissions it asks for, the IR schema it speaks and the
minimum Molto it needs. A plugin is a native executable, so its `target` **MUST
NOT** be `any`.

**A plugin recipe is the one place the unknown-key rule below does not apply.**
A reader that ignores a `[plugin]` table it does not understand runs an
executable under permissions it never saw, which is the opposite of what
ignoring unknown keys is for. A recipe with `kind = "plugin"` therefore
declares a higher `schema`, so a Molto that does not know about plugins refuses
it outright instead of reading the half it recognises.

### `[package]`

Required for `kind = "package"`. Optional string lists `include`, `link`,
`defines` and `flags`, with the same meaning as in `[artifacts]` — for a binary
package recipe the two are the same table, and `[package]` is the historical
spelling. An optional `[deps]` table declares the package's own dependencies,
in exactly the syntax RFC-0003 specifies for a manifest, **exact versions
included**: a recipe may not name a range any more than a manifest may
(RFC-0008). This is what lets a resolver walk the graph over metadata alone.

### `[about]`

Optional for every kind. `description`, `license`, `homepage`, `repository`,
`authors`. Purely informational, served in the catalogue so a search result can
say what something is — and read by whatever has to name the licence of every
package in a build.

RFC-0003 specifies the same five keys under `[package]` in a manifest, and the
two **MUST** agree, since publishing a Molto package derives one from the other.
One reader answers for both tables, parameterised by the table's name, so the
two cannot come to disagree about what a key means. `license` is checked for
shape as an SPDX expression and not against the identifier list; RFC-0003 gives
the reason.

## Canonical recipes

Everything above is a table of keys, and a table of keys is not a document. The
five recipes below are **normative**: each is valid exactly as written, each
covers one case the ecosystem has, and a conforming implementation should accept
all five and be tested against them.

### The minimum

Every required key and nothing else. A header-only library that needs no build
and exports one include directory:

```toml
schema = 1
form = "binary"
kind = "package"
name = "stb_image"
version = "2.30.0"
target = "any"

[package]
include = ["include"]
```

### A toolchain

A compiler that already exists and is only being distributed. `provides` is the
vocabulary a manifest's `[target].requires` asks for, which is how pickup
answers "which local compiler gives me `attr_nodiscard`" without any manifest
naming a binary:

```toml
schema = 1
form = "binary"
kind = "toolchain"
name = "gcc"
version = "14.2.0"
target = "x86_64-unknown-linux-gnu"
provides = ["attr_nodiscard", "c23", "cxx20"]

[toolchain]
vendor = "gnu"
triple = "x86_64-unknown-linux-gnu"
c_driver = "bin/gcc"

[toolchain.c]
std = ["c11", "c17", "c2x", "c23"]
compile_flags = []
link_flags = []
runtime_dirs = ["lib", "lib64"]

[toolchain.cxx]
std = ["c++17", "c++20", "c++23"]
runtime_dirs = ["lib", "lib64"]

[about]
description = "The GNU Compiler Collection"
license = "GPL-3.0-or-later"
homepage = "https://gcc.gnu.org"
```

### A tool

What `molto fmt` and `molto lint` obtain from the registry. `binary` is the path
inside the archive; `aliases` are the other names the tool answers to, so a
`format.json` naming `clang-format` finds it:

```toml
schema = 1
form = "binary"
kind = "tool"
name = "clang-format"
version = "19.1.0"
target = "x86_64-unknown-linux-gnu"

[tool]
kind = "formatter"
binary = "bin/clang-format"
aliases = ["clang-format-19"]

[about]
description = "The LLVM code formatter"
license = "Apache-2.0 WITH LLVM-exception"
```

### A binary package

A Molto package published as a prebuilt static library, with a dependency of its
own. Note the exact version under `[deps]`:

```toml
schema = 1
form = "binary"
kind = "package"
name = "http"
version = "0.2.0"
target = "x86_64-unknown-linux-gnu"

[package]
include = ["include"]
link = ["http"]
defines = ["HTTP_STATIC"]
flags = []

[deps]
yyjson = "0.10.0"

[about]
description = "A small HTTP client"
license = "MIT"
repository = "https://github.com/example/http"
```

### A source package

The case `spec.md` §8 was written for: a library that predates every package
manager that would want it, obtained and built on the consumer's machine.
`[build]` names an existing build system and passes it arguments; it does not
carry a script:

```toml
schema = 1
form = "source"
kind = "package"
name = "libpng"
version = "1.6.40"
target = "any"

[source]
archive = "https://download.sourceforge.net/libpng/libpng-1.6.40.tar.gz"
sha256 = "8f720b363aa08fed695ebe0e2b4e6ada6f0b5f4b1e3e05bdc0f5c9d9b4c72c99"
strip_prefix = "libpng-1.6.40"

[build]
system = "autotools"
args = ["--disable-shared", "--enable-static"]
jobs = true

[build.env]
CFLAGS = "-fPIC"

[artifacts]
type = "static"
include = ["include"]
link = ["png16"]

[deps]
zlib = "1.3.1"

[about]
description = "The reference library for the PNG image format"
license = "libpng-2.0"
homepage = "http://www.libpng.org/pub/png/libpng.html"
```

## A real one: `sqlite`

The five above are shaped to teach the format. This one is shaped by a real
library, and it is the recipe the ecosystem actually needs first — `sqlite` is
the dependency `spec.md` §7 has used as its example since the beginning.

SQLite is the interesting case precisely because it is not hard: it ships as a
single amalgamated `.c` file, so there is no build system to name at all. That
is what `system = "none"` and `type = "source"` are for — the consumer compiles
it as if it were its own code, which is also how SQLite's own documentation
tells people to use it.

```toml
schema = 1
form = "source"
kind = "package"
name = "sqlite"
version = "3.53.4"
target = "any"

[source]
archive = "https://sqlite.org/2026/sqlite-amalgamation-3530400.zip"
sha256 = "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d"
strip_prefix = "sqlite-amalgamation-3530400"

[build]
system = "none"

[artifacts]
type = "source"
sources = ["sqlite3.c"]
include = ["."]
link = ["m", "dl", "pthread"]
defines = ["SQLITE_THREADSAFE=1", "SQLITE_ENABLE_FTS5", "SQLITE_ENABLE_RTREE"]

[about]
description = "A small, fast, self-contained SQL database engine"
license = "blessing"
homepage = "https://sqlite.org"
```

Four things this recipe settles that the tables alone leave open:

- **`link` may name system libraries.** `m`, `dl` and `pthread` are not
  dependencies to resolve — they are `-l` flags the consumer's link line needs,
  and RFC-0008 is explicit that Molto does not model what the platform already
  provides.
- **`defines` are part of the ABI, not a preference.** Compiling SQLite with
  `SQLITE_THREADSAFE=1` in one translation unit and not in another produces a
  library that mostly works. Because they live in the recipe, every consumer
  gets the same set, and changing the set is a new version.
- **`include = ["."]`** is how an amalgamation exports itself: the root of the
  unpacked source is the include directory, because the header sits next to the
  `.c`.
- **`sources` is not optional in practice.** The archive also carries `shell.c`
  and its `main()`. Without the list the consumer links two of them, and the
  first thing anyone learns about this recipe is a duplicate symbol.

The `sha256` above is worth a note, because getting it wrong is easy and the
mistake is invisible: sqlite.org publishes a **SHA3-256** on its download page,
and both digests are sixty-four hex characters, so one reads exactly like the
other. They answer different questions and a recipe needs both, at different
times. Upstream's digest verifies that the file the publisher downloaded is the
file upstream released; the recipe's `sha256` verifies that every consumer
fetches the bytes the publisher verified. A publisher checks the first by hand,
once, and writes the second — which is why this RFC asks for one algorithm
rather than for whichever one an origin happens to prefer.

One thing it settles that is not in the recipe at all: the digest is **not** the
hash sqlite.org prints on its download page. That page publishes SHA3-256 and
this format carries SHA-256, so copying the published figure produces a recipe
that fails verification on every machine. The two are worth cross-checking
against each other, which is the only reason to have both.

## Compatibility

`schema` is an integer, present from the first version, and this is the one
place this RFC departs from RFC-0003 — which reserved a manifest version key
rather than defining one. The reason is distribution. A `Project.toml` is read
by the Molto on the machine that wrote it, so a mismatch is a local problem
solved by upgrading. A recipe is published once and read for years by clients
of every version, including versions that did not exist when it was written. It
has to state the contract it was written against, and it cannot be added later
without a flag day.

The rules:

- **Unknown keys are ignored.** A recipe may carry keys a reader does not know,
  and a reader must not fail on them. This is what allows the format to grow.
- **An unknown `kind` is an error.** The kind selects the validator; guessing
  would publish an artifact into a catalogue nothing looks in.
- **A `schema` higher than the reader supports is rejected**, not interpreted.
  A newer format may give an existing key a new meaning, and a reader that
  proceeds anyway is confidently wrong. Rejecting says "upgrade Molto", which is
  a fixable error; guessing produces a broken build with no message.
- **`schema` increments only for changes that break a reader.** Adding an
  optional key does not; changing a default, removing a key, or redefining one
  does.

## Implementation Status

What a registry validates today (`recipe.ts`): `kind`, `name`, `version` and
`target` with the patterns above, the per-kind table for each of the three
kinds, `[toolchain]`'s required strings and its optional per-language lists,
`[tool]`'s role and binary, and `[package]`'s four string lists.

Since molto 0.4.0 and registry 0.2.0, both modes exist end to end for
publication: `schema` and `form` are read and validated, `[source]`, `[build]`
and `[artifacts]` are validated for a source recipe, `molto publish` sends one
without looking for an archive, and the registry stores it with no blob (its
`artifacts` table carries a `form` column and nulls the four blob fields for
one). `source_service` fetches a `[source]` — archive with its digest verified,
git at a rev, or a local path — into a cache addressed by coordinate.

What is specified here and consumed nowhere:

- **`[build]` and `[artifacts]` at build time.** They are validated and stored,
  and nothing acts on them: no build system is run, and nothing puts `include`,
  `link`, `defines` or `sources` on a compile line. This is the largest gap in
  this RFC and it is blocked on the rest of RFC-0008 — a resolver to decide
  *that* a dependency is wanted, which the fetcher and the recipe then serve.
- **`[deps]` inside a package recipe.** It is checked to be a table and nothing
  more — its keys, sources and constraints are unvalidated. Since a registry
  cannot resolve versions (RFC-0010) it cannot check satisfiability, but it can
  and should check the shape, and today it does not.
- **`[about]` and `package.std`.** Both appear in the registry's published
  examples and neither is validated or specified anywhere else. `package.std` is
  not adopted by this RFC: a standard belongs to a build, and a binary artifact
  has already been built.

## Non-Goals

A recipe is not a build script. It names a build system and gives it arguments;
it cannot run commands, patch sources, or branch on the host. A dependency that
genuinely needs those is a dependency that needs a fork.

A recipe does not describe how to *install* anything system-wide. Artifacts land
in Molto's cache and on compile lines; nothing is written outside the user's
Molto directories.

A recipe is not a substitute for a manifest. A project that could be a Molto
package should be one, and should publish as `kind = "package"` with a real
`Project.toml` behind it.

## Reserved / Future

- **Patches.** A `[[patch]]` list applied between `[source]` and `[build]` is
  the obvious next request and the obvious way to reintroduce arbitrary
  behaviour. It waits until source recipes exist and the need is concrete.
- **Per-target overrides**, so one recipe can describe a build that differs on
  Windows without becoming three recipes.
- **Feature selection**, once RFC-0003's `[features]` is un-reserved.
- **Signing.** A recipe's integrity currently rests on the registry's checksum;
  a signature would let it rest on the publisher instead.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md) — why a recipe names a build system instead of inventing one
- [RFC-0002: CLI Specification](0002-cli-specification.md) — `molto publish`, which reads a recipe
- [RFC-0003: Project Manifest](0003-project-manifest.md) — the `[deps]` syntax a recipe reuses, and the reserved metadata keys `[about]` mirrors
- [RFC-0005: Code Style](0005-code-style.md) — the formatters and linters a `tool` recipe distributes
- [RFC-0007: Build System](0007-build-system.md) — where `[artifacts]` ends up on a command line
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — how a source recipe is reached, and why by version
- [RFC-0010: Registry Specification](0010-registry-specification.md) — how a recipe is published and served
- [RFC-0014: Plugin System](0014-plugin-system.md) — the `[plugin]` table, and the frontend `via = "frontend"` hands a build to

See also `spec.md` sections 8 (Recipes) and 9 (Artifacts).
