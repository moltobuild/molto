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

It formalises `spec.md` section 8, gives RFC-0008 the meaning of its `recipe`
dependency source, and writes down the contract a registry already validates
(RFC-0010).

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
  is the form RFC-0008's `recipe` dependency source resolves to.

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
resolved by a version requirement must be semver, because RFC-0008 has to order
it.

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

An `archive` without a `sha256` is invalid. A URL is a promise about a location,
not about content, and a recipe whose output changes when upstream re-rolls a
tarball is not a recipe — it is a suggestion. `git` needs no digest because a
commit id is one.

## `[build]`

Source recipes only. How the thing is built — by naming a build system, never by
carrying a script.

| Key | Type | Description |
|---|---|---|
| `system` | string | `make`, `cmake`, `autotools`, `meson` or `none` |
| `args` | array[string] | Arguments passed to it, verbatim |
| `env` | table | Environment variables set for the build |
| `jobs` | bool | Whether the build system is given a parallelism flag |

`system = "none"` is for a source drop that needs no build: headers, or sources
compiled by the consumer as if they were its own.

This table is where the temptation to invent a build language lives, and the
answer is no (RFC-0001, and RFC-0007's Non-Goals). A recipe that could run
arbitrary commands would make every dependency a remote code execution with
extra steps, and would make Molto the thing it says it is not. Naming an
existing build system means the escape hatch is `args`, which is bounded, and
means the recipes people write stay readable — `system = "cmake"` says more
about a dependency than twenty lines of shell.

A build system Molto does not know is a rejected recipe, not a fallback to
`sh -c`.

## `[artifacts]`

What the consumer gets. This is the join with RFC-0007: everything declared here
ends up on a compile or link line of the project that depends on it.

| Key | Type | Description |
|---|---|---|
| `type` | string | `source`, `static` or `shared` (default `static`) |
| `include` | array[string] | Directories added as `-I`, relative to the artifact root |
| `link` | array[string] | Libraries added as `-l` |
| `defines` | array[string] | Defines added as `-D` |
| `flags` | array[string] | Raw flags a consumer must compile with |

`flags` is the escape hatch and, as in RFC-0003, it is passed verbatim and is
the least portable thing a recipe can contain. A recipe that needs it for
anything other than a genuine ABI requirement — `-pthread`, say — is usually
describing something that belongs in `defines`.

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

Required for `kind = "tool"`. `kind` is one of `formatter`, `linter` or
`linker`, and `binary` names the executable inside the archive. An optional
`aliases` string list gives the other names the tool is known by. This is what
lets `molto fmt` and `molto lint` obtain their tools from the same registry
that serves libraries (RFC-0005).

### `[package]`

Required for `kind = "package"`. Optional string lists `include`, `link`,
`defines` and `flags`, with the same meaning as in `[artifacts]` — for a binary
package recipe the two are the same table, and `[package]` is the historical
spelling. An optional `[deps]` table declares the package's own dependencies,
in exactly the syntax RFC-0003 specifies for a manifest.

### `[about]`

Optional for every kind. `description`, `license`, `homepage`, `repository`.
Purely informational, served in the catalogue so a search result can say what
something is. RFC-0003 reserves the equivalent keys for `[package]` in a
manifest; when it un-reserves them, the two **MUST** agree, since publishing a
Molto package derives one from the other.

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

What is specified here and validated nowhere:

- **`schema` and `form`.** Neither key exists yet, which is why there is only
  one mode in practice. `form` is the cheaper of the two to add and the more
  urgent, because without it a source recipe cannot be told from an incomplete
  binary one.
- **`[source]`, `[build]` and `[artifacts]`.** No source recipe can be
  published, resolved or built. This is the largest gap in this RFC and it is
  blocked on RFC-0008: a source recipe with no dependency resolution to consume
  it would be a document nothing reads.
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

- [RFC-0001: Manifesto](0001-manifesto.md) — why a recipe names a build system instead of running one
- [RFC-0002: CLI Specification](0002-cli-specification.md) — `molto publish`, which reads a recipe
- [RFC-0003: Project Manifest](0003-project-manifest.md) — the `[deps]` syntax a recipe reuses, and the reserved metadata keys `[about]` mirrors
- [RFC-0005: Code Style](0005-code-style.md) — the formatters and linters a `tool` recipe distributes
- [RFC-0007: Build System](0007-build-system.md) — where `[artifacts]` ends up on a command line
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — the `recipe` dependency source this document defines
- [RFC-0010: Registry Specification](0010-registry-specification.md) — how a recipe is published and served

See also `spec.md` sections 8 (Recipes) and 9 (Artifacts).
