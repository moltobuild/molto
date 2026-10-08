# RFC 0024: Molto Packages as Dependencies

- RFC Number: 0024
- Title: Molto Packages as Dependencies
- Status: Accepted
- Created: 2026-10-06

## Summary

A `git` or `archive` dependency whose source is a Molto project is consumed
from its `Project.toml` alone. The manifest gains one table, `[interface]`, for
what the package gives its consumers; `[target]` becomes what it keeps to
itself. Molto derives the dependency's `[artifacts]` from the two, compiles
`src/` and nothing else, keeps in the shared store only the files a consumer
needs, and never lets a dependency's `main()` collide with the consumer's.

A new command, `molto package`, shows an author what a consumer will receive
and proves that it builds, before anyone depends on it.

Recipes (RFC-0009) are unchanged and remain the way to offer, through the
registry, a project that is not built with Molto. Publishing a Molto package to
the registry from its manifest is left to a later RFC.

## Motivation

Every Molto package consumed through `git` has to carry a `recipe.toml` beside
its `Project.toml` (`dep_graph.c`, `read_carried_recipe`), because `[deps]` has
nowhere to say what to compile. In moltest, moltest-coverage and moltest-mock
the recipe restates the manifest: the name, the version, the standard, the
include directory, the licence. Two files that must agree, and nothing checks
that they do.

The recipe says only three things the manifest cannot:

1. **Which sources.** A recipe lists them; a project discovers `src/`.
2. **What is interface and what is private.** `_DEFAULT_SOURCE` is moltest's
   interface, `__USE_MINGW_ANSI_STDIO=1` is private; `--coverage` is
   moltest-coverage's interface. `[target]` is one flat list.
3. **How it is consumed.** `type = "source"` where the manifest says
   `artifact = "static"`.

Closing those three gaps makes the recipe redundant for a Molto project.

Two further problems exist today and are fixed here because they become worse
once any repository can be a dependency:

- **The store keeps everything.** A git dependency is a full `git clone`:
  history, `tests/`, `docs/`, CI configuration. None of it is compiled.
- **A dependency's `main()` is linked loose.** A runtime dependency that ships
  a `main()` is a duplicate symbol in every consumer. moltest's `main()` works
  only because a `mode = "single"` suite defines none; a `per_file` suite with
  moltest as a dev-dependency fails to link today.

## Which file describes a fetched source

At the root of a fetched `git` or `archive` source:

| `recipe.toml` | `Project.toml` | Result                                               |
|---------------|----------------|------------------------------------------------------|
| yes           | no             | The recipe, as today (RFC-0009).                     |
| no            | yes            | The manifest, as this RFC specifies.                 |
| yes           | yes            | **Error.** Both files claim to describe the package. |
| no            | no             | Error naming both files.                             |

Both files present is an error rather than a precedence rule: whichever one
won, the other would describe something that is not built, and the author would
be the last to find out. Tags published before this RFC keep working with older
consumers, and with this one as long as they carry only the recipe.

A `path` dependency follows the same table. It is never pruned (see below).

## `[interface]`

```toml
[interface]
include = ["generated"]         # -I for consumers besides include/, see below
defines = ["_DEFAULT_SOURCE"]   # -D for consumers
flags   = ["--coverage"]        # verbatim, compile and link line of consumers
link    = ["m"]                 # -l for the final binary
entry   = "src/moltest_main.c"  # a main() offered to consumers, see below
```

Every key is optional. An unknown key is an error, as in `[package]`: a
misspelled interface key silently drops something every consumer needed.

### `include/` is the interface by convention

When the package has an `include/` directory at its root, consumers receive it
as `-I` without the manifest saying so, as `src/` and `tests/` are discovered
without being declared (RFC-0001). The package's own build still needs
`[target].include = ["include"]`, which is where it already is in every Molto
library; it is not repeated under `[interface]`, and an `[interface].include`
naming it is an error rather than a second spelling of the same directory.

`[interface].include` is only for what lies elsewhere: a generated header
directory, a second public tree. A header that consumers must not see belongs
in `src/`, which is never on their include path. `[target].include` entries
are private, with `include/` the one exception the convention makes.

### What a consumer gets

The derived `[artifacts]` (RFC-0009) are:

| `[artifacts]`              | From                                                         |
|----------------------------|--------------------------------------------------------------|
| `type`                     | `source` for `artifact = "static"` or `"shared"`             |
| `std`, `cpp_std`           | `[target].std`, `[target].cpp_std`                           |
| `sources`                  | every source under `src/`, except `src/main.c`               |
| `include/defines/flags`    | `include/` when it exists, plus `[interface]`                |
| `link`                     | `[target].link` and `[interface].link`                       |
| `private.include/defines/flags` | `[target].include/defines/flags`, without `include/`    |
| `[deps]`                   | `[deps]`                                                     |
| `[about]`                  | `[package]` (`manifest_read_about`, as both already share)   |
| version                    | `[package].version`                                          |

`[target].requires` and `[target].host` (RFC-0016) are carried to the
consumer's toolchain resolution as well: a package that needs
`attr_nodiscard` needs it in whatever compiles it.

`[dev-deps]`, `[test]`, `[profile.*]`, `[env]` and `[plugins]` of a dependency
are ignored. A dependency's plugins in particular never run: RFC-0014's consent
is given by the project that names a plugin, not by one of its dependencies.

`artifact = "executable"`, the default, is refused as a dependency. An
executable has nothing to link, and consuming one by accident would compile its
`main()` into someone else's binary.

The dependency's name in the consumer's `[deps]` must equal its
`[package].name`. When the dependency is a `tag`, a version that disagrees with
the tag (`v0.3.0` against `0.3.1`) is a warning.

## `main()` in a dependency

Two rules, for the two reasons a library has a `main()`.

**`src/main.c` is the package's own program** (the convention RFC-0002 and the
test build already apply to the root, `build_tests.c`). A library that also
ships a command-line tool keeps it there. It is never compiled for a consumer,
and it is pruned.

**`[interface].entry` is a `main()` offered on purpose.** moltest's
`src/moltest_main.c` exists so a suite needs no `main()` of its own. The entry
file is compiled into an archive of its own, `<dep>.entry.a`, placed after
every object on the link line. A linker takes a member from an archive only to
resolve a symbol still undefined, so:

- the consumer defines `main()`: the entry is never taken, and nothing collides;
- the consumer does not (a `single` suite): the entry provides it.

This holds for GNU ld, ld64, lld and `link.exe` alike. The entry file must
contain `main()` and nothing a consumer relies on otherwise: a constructor in it
is dropped whenever the entry is not taken. Registration constructors, like
moltest-coverage's `register.c`, belong in `src/` and stay linked loose.

Two dependencies reachable from the same binary that both declare an `entry`
are an error naming both, rather than whichever the linker meets first.

A `main()` in any other source of a dependency cannot be found portably before
the link. When the link then fails on a duplicate `main`, the diagnostic
(RFC-0011) names the dependency and points at `[interface].entry`.

## Pruning the store

After a `git` or `archive` dependency described by its `Project.toml` is
fetched, and **before** it is moved into the store, everything not on this list
is deleted from the working directory:

- `Project.toml`;
- `src/`, except `src/main.c`;
- `include/`, and the directories named by `[target].include` and
  `[interface].include`;
- the `[interface].entry` file;
- `LICENSE*`, `NOTICE*`, `COPYING*` at the root, which are never optional:
  Apache-2.0 requires NOTICE to travel with the code, and RFC-0003 requires a
  resolved graph to name the licence of what it links;
- whatever `[package].files` names.

It is a list of what stays rather than of what goes: a directory added
tomorrow is not kept by accident. Pruning before the install keeps the install
atomic (`source_service.c`, `install`), so the store never holds a half-pruned
tree marked complete. `.git/` goes with the rest; nothing reads it after the
fetch.

The fetch itself shrinks too: the resolved commit is fetched with
`git fetch --depth 1`, falling back to a full clone when the host refuses to
serve a commit by its hash.

The completion stamp records the pruning rules' version. A store entry pruned
by older rules is fetched again rather than trusted, so a later Molto that keeps
more files never meets a tree that is missing them.

**Never pruned:** a `path` dependency, which is the user's own directory; and a
source described by a recipe, whose `[[provide]]` and configure step may need
files anywhere in it.

### `[package].files`

```toml
[package]
files = ["third_party/**", "data/tables.inc"]
```

Paths and globs, relative to the package root, kept in addition to the list
above. It is what a package needs when a source includes
`../third_party/x.h` or `#embed`s a file outside `src/`. A pattern matching
nothing is an error in `molto package`, where a typo is cheap to fix.

## `molto package`

The author's side of all of the above. A pruned package that misses a file
compiles in its own repository and fails only for its consumers, so the author
needs a way to see the failure first.

```
molto package [--list]
```

1. **Checks the manifest** against the checklist below, and prints it.
2. **Assembles the package** in `build/package/<name>-<version>/` with the same
   pruning function the store uses. The input is the files git tracks
   (`git ls-files`), not the working directory: a header that `.gitignore`
   hides exists here and not in the clone. Uncommitted changes to tracked
   files are a warning, because the tag will not have them.
3. **Builds it as a consumer would**: the derived `[artifacts]`, compiled from
   the assembled copy, with the interface and the private options split as a
   consumer splits them. A missing header fails here.
4. **Reports** what is kept, how much, and how much was pruned.

`--list` stops after step 2 and prints the kept files.

Exit status is non-zero when a required item fails or the build does. It is
meant for each package's CI.

### The checklist

The same checks back the error a consumer sees when a fetched `Project.toml`
does not qualify, so an author and a consumer read the same words.

| Check                                                     | Level   |
|-----------------------------------------------------------|---------|
| `[package].name` and `version` are present                | error   |
| `artifact` is `static` or `shared`                        | error   |
| `src/` holds a source other than `src/main.c`             | error   |
| every `include` directory and the `entry` file exist      | error   |
| `entry` is not `src/main.c`                               | error   |
| no `recipe.toml` beside `Project.toml`                    | error   |
| every `[package].files` pattern matches something         | error   |
| `[package].license` is set and a licence file is present  | warning |
| `[interface].include` does not name `include/`            | error   |
| `include/` exists, or `[interface].include` names a directory | warning |
| `[package].repository` is set                             | warning |

When `molto package` passes, it ends with what a consumer adds:

```
molto: moltest 0.3.1 is ready to be a dependency
  kept 9 files (84 KiB), pruned 61 (1.9 MiB)
  consumers add: moltest = { git = "https://github.com/moltobuild/moltest", tag = "v0.3.1" }
```

On the consumer's side, a fetched manifest that fails a required item is an
error listing every failed item and ending with: "its author can run
`molto package` to see what a dependency needs".

## Migration

1. Release Molto with this RFC implemented.
2. moltest: `[interface] defines = ["_DEFAULT_SOURCE"]`,
   `entry = "src/moltest_main.c"`. moltest-coverage:
   `[interface] flags = ["--coverage"]`. moltest-mock needs no `[interface]`:
   its `include/` is exported by convention.
3. Each runs `molto package` in CI, deletes `recipe.toml`, and tags a release.
4. Molto, moltest-coverage and moltest-mock move their `[dev-deps]` pins to
   those tags.

## Changes to other RFCs

- RFC-0003: `[interface]`; `[package].files`; `[target]` is private when the
  package is a dependency.
- RFC-0008: a carried dependency is described by its recipe or its manifest,
  never both; pruning of the store.
- RFC-0009: a recipe is no longer required for a Molto package fetched by
  `git`, `archive` or `path`.
- RFC-0002: `molto package`.

## Non-goals

- Publishing a Molto package to the registry from its manifest: a later RFC.
- Pruning, or an `entry`, for a source described by a recipe.
- Prebuilt artifacts: a dependency is still compiled by its consumer.
- `[target.<os>]` in a dependency: it maps onto `[artifacts.<os>]` once
  RFC-0003's per-OS tables are built, and not before.

## Related RFCs

- [RFC-0002: CLI Specification](0002-cli-specification.md)
- [RFC-0003: Project Manifest](0003-project-manifest.md)
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md)
- [RFC-0009: Recipe Specification](0009-recipe-specification.md)
- [RFC-0011: Build Diagnostics](0011-build-diagnostics.md)
- [RFC-0014: Plugin System](0014-plugin-system.md)
- [RFC-0016: Host Libraries](0016-host-libraries.md)
- [RFC-0021: Isolated Tests](0021-isolated-tests.md)

## Implementation Status

The manifest reader, carried dependency conversion, source-only discovery,
private/interface split, optional entry archives, pre-install pruning,
versioned stamps, shallow git fetch with fallback and `molto package [--list]`
are implemented. See [the package guide](../docs/Packages.md).

Acceptance coverage lives in `tests/services/test_package_service.c`:

- [x] Public/private options and source discovery, excluding the package main.
- [x] Strict interface keys, conventional includes and aggregated checklist errors.
- [x] Dependency development configuration is ignored; duplicate descriptions fail.
- [x] Paths remain unpruned; archives retain the allowlist and invalidate old stamps.
- [x] Recipe-directed sources keep their complete upstream tree.
- [x] Entry archives support consumers and test binaries with their own main.
- [x] Two entries are refused; author assembly uses tracked files and rejects missing headers.

Ecosystem migration PRs remove the carried recipes from moltest, coverage and
mock. This repository pins their immutable migrated revisions while compatible
release tags are pending. Replace the revision pins after those releases.
