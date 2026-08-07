# RFC 0007: Build System

- RFC Number: 0007
- Title: Build System
- Status: Draft
- Created: 2026-08-07

## Summary

This RFC specifies how Molto turns a directory into a binary: how sources are
**discovered**, what the **build graph** is, when a translation unit is
**recompiled**, where **artifacts** are stored, how the **link** line is
composed, what a **profile** changes, and how the work is **parallelised**.

It formalises `spec.md` sections 6 (the filesystem describes the project), 12
(incremental compilation), 13 (build profiles) and 20 (performance). RFC-0004
specifies *where* incremental state lives; this RFC specifies what the build
does with it. RFC-0003 specifies the manifest keys that feed it; this RFC
specifies what those keys produce on a command line.

## Motivation

The build is the oldest part of Molto, the only one that has never been
written down, and the one every other feature depends on. It compiles itself
today, which means the contract is real; it also means the contract exists
only as C, and the only way to answer "does a `touch` recompile?" or "do
profile flags reach the linker?" is to read `build_service.c`.

That is not sustainable for three reasons. Dependency resolution (RFC-0008)
will add edges to something that currently has none, and it needs to know what
it is extending. A second implementation of the ecosystem — a plugin, a
different front-end — has nothing to conform to. And the parts that are *not*
built (the global cache, `static`/`shared` artifacts, `molto bench`) are
indistinguishable from the parts that are, so nobody can tell a gap from a
decision.

## Discovery

The tree is the file list. A project declares no sources, and Molto **MUST NOT**
require it to (RFC-0001, Philosophy).

Two directories are walked, recursively and without a depth limit:

- `src/` — the package's own sources. It is mandatory: a project without it, or
  with an empty one, is a build error rather than a build that produces nothing.
- `tests/` — the test sources. It is optional; its absence means a project has
  no tests, which is a fact, not a failure.

A file joins the source set when its extension is `.c`, `.cpp` or `.cc`. A
`.cpp` or `.cc` file is C++ and everything else is C — the extension is the only
signal, because the alternative is a manifest key that repeats what the
filesystem already says.

Formatting and linting need a wider set: the same walk plus `.h`, `.hpp` and
`.hh`, since RFC-0005 applies style to a header exactly as to a source. That is
the only difference between the two collections.

Two rules make the walk safe and reproducible:

- **Symlinked directories are not followed**, though symlinked files are
  collected. A tree that links to its own ancestor is a mistake a build must
  survive, not hang on.
- **The result is sorted lexicographically by full path.** Readdir order is a
  filesystem detail and differs between machines. Since the object list becomes
  the link line, an unsorted walk would produce link lines that differ across
  machines for identical inputs, and reproducible builds are a stated goal.

`[test].sources` (RFC-0003) extends the test set with extra directories or
individual files, for suites whose sources live outside `tests/`. A directory is
walked; a file is taken as it is; a relative path is anchored at the workspace
root. **An entry that does not exist is a manifest error**, not a warning: the
manifest asserted that a path is part of the project, and a build that quietly
compiles less than it was told to is worse than one that stops.

## The build graph

Molto has no build graph. It has a flat list of independent translation units
and, per object, the list of files that object was compiled from.

That is an honest description of the implementation and, for now, a sufficient
one. A graph earns its keep when there are edges to order — when target B cannot
start until target A has produced a header or a library. A single package has no
such edges: every translation unit in `src/` can compile at the same instant,
and the only ordering in the whole build is "all objects, then the link".

The per-object prerequisite list comes from the compiler. Each compile is issued
with `-MMD -MF <object>.d`; the resulting depfile is parsed, absorbed into the
WSDB, and then deleted, so the dependency information lives in one store rather
than beside the objects. The parse handles line continuations and escaped
spaces, and discards the target, which is already known.

`-MMD` and not `-MD`: **system headers are deliberately not tracked**. A change
to `stdio.h` is not a file someone edited, it is a different toolchain, and a
different toolchain changes the resolved compiler recorded in the WSDB
(RFC-0004) — which is itself a tracked input. Tracking every system header
individually would multiply prerequisite lists by an order of magnitude to
detect an event that is already detected.

What a real graph will have to add, when RFC-0008 lands: targets as nodes rather
than three hard-coded shapes, a topological order over package dependencies,
cycle detection, and a reverse index from header to dependent unit — today
freshness is answered by walking every object's prerequisites, which is fine for
hundreds of units and will not be fine for thousands.

## Incremental compilation

An object is **fresh**, and its compile is skipped, when all four hold:

1. the WSDB has an object entry for it;
2. the recorded command string is byte-identical to the command this build would
   issue;
3. the object file can be `stat`-ed;
4. every recorded prerequisite is unchanged.

The command fingerprint is **the entire command line**, joined by spaces. There
is no list of "flags that matter", because every such list is eventually wrong:
it omits the one flag someone adds next. Recording the whole line means a
changed `-D`, a changed `-std`, a changed optimisation level, a changed include
path, and a changed compiler driver all invalidate the object by construction,
without anyone having enumerated them.

A prerequisite is **unchanged** under a hybrid test. Nanosecond mtime and size
are compared first; if both match, the file is unchanged and nothing is read. If
either differs, the content hash decides — and when the hash matches, the stored
mtime and size are refreshed so the fast path works next time. The consequence
is the one users notice: `touch` does not trigger a rebuild, and neither does a
checkout that rewrites timestamps, but an edit that reverts a file to its
previous content correctly stops being a change.

Prerequisites include the source, every header the compiler reported, and the
compiler binary itself. Recording the compiler as an input is what makes
"the toolchain changed" a rebuild rather than a silent mismatch.

### A source edited mid-compile

A file can be saved while its own compile is running. The object produced then
corresponds to neither version, and if it were recorded as fresh it would stay
wrong until something else invalidated it — a stale object that survives every
subsequent build is the worst failure this machinery can produce.

The freshness signature of each source is therefore captured before its compile
starts and compared again after it finishes. If it moved, **the object is not
recorded**. The next build recompiles it. The cost is one wasted compile; the
alternative is a binary that does not match its sources and no way to tell.

## Three stores, and the one that is missing

Three different things are called "the cache", and conflating them is the
easiest way to reason wrongly about this system.

| Store | Contents | Scope | Safe to delete |
|---|---|---|---|
| `build/<profile>/` | objects and binaries | one project, one profile | yes, forces a full rebuild |
| `.bin/wsdb` | incremental state (RFC-0004) | one workspace | yes, forces a full rebuild |
| `~/.molto/` | reusable artifacts (`spec.md` §9-10) | all projects | yes, forces re-fetch |

`build/<profile>/obj/` mirrors the source tree, so `src/a.c` becomes
`build/debug/obj/src/a.c.o`. The mirror keeps object paths unique without
hashing and keeps them legible, and because the profile is a path segment,
`debug` and `release` outputs coexist instead of invalidating each other.

The global cache does not exist. `~/.molto/` currently holds one file,
`credentials.toml`. Two rules govern it when it is built:

- **Reusable build artifacts are cached; source repositories never are**
  (`spec.md` §9). A cloned repository is an input the user could have cloned
  themselves, it is large, and it is worthless to a second project unless it is
  at the same revision — at which point what is worth keeping is what was built
  from it, not it.
- **It must be addressed by content, not by path.** The local cache is keyed by
  output path, which is exactly why it can never be shared: two projects that
  compile identical sources with identical flags write to two different paths
  and cannot see each other's work. A global cache keyed by a hash of its inputs
  can. Reusing the local scheme globally would produce a cache that never hits.

## Linking

Objects are linked with the compiler driver, not with `ld` directly, so the
driver supplies the C or C++ runtime and the startup files. The driver is the
C++ one when **any** translation unit in the target is C++, and the C one
otherwise; if C++ is needed and none was resolved, the build stops and says so
rather than linking with a driver that will fail obscurely.

The line is composed in this order:

```
<driver> <objects…> <[target].flags…> <[profile].flags…> -o <binary> -l<lib>…
```

Profile flags reach the linker on purpose. `-flto` and `-fsanitize=…` are not
compile-only options: they change what the link step must do, and passing them
at compile time alone produces objects the link cannot handle. Defines and
include paths are *not* passed, because they mean nothing to a link.

Relinking happens when something recompiled, when the binary is missing or older
than an object, or when the link command's fingerprint changed — the same
fingerprint rule as for objects, which is what makes an added `-l` or a removed
source relink without any special case.

Test binaries link the objects of `src/` **minus `src/main.c.o`**, plus the test
objects. Excluding the package's `main` is what allows the suite to provide its
own, and it is why a project's library code is testable without being split into
a separate target first.

The output is `build/<profile>/<package-name>`. There is exactly one executable
per package.

RFC-0003 defines `[package].artifact` as `source`, `static` or `shared`. Molto
**rejects the manifest** that sets it, with any value, rather than accepting the
key and ignoring it. Producing a `.a` requires `ar`, and a `.so` requires
`-fPIC`, a soname and a versioning policy; none of that exists. An explicit
refusal costs the user one error message, while silent acceptance costs them a
debugging session over a shared library that was never built.

## Profiles

A profile is a named set of compile settings and a directory segment. Four names
exist: `debug`, `release`, `bench` and `custom`. They are selected with
`--profile` on `build`, `run`, `test` and `lint`, and default to `debug`. An
unknown name is a usage error.

Built-in defaults apply when the manifest declares nothing:

| Profile | `opt_level` | `debug_info` |
|---|---|---|
| `debug` | 0 | true |
| `release` | 3 | false |
| `bench` | 3 | false |
| `custom` | 2 | true |

`[profile.<name>]` overrides them and adds to the base:

| Key | Type | Effect |
|---|---|---|
| `opt_level` | integer | `-O<n>` |
| `debug_info` | bool | `-g` when true |
| `defines` | array[string] | `-D<value>`, appended after `[target].defines` |
| `include` | array[string] | `-I<value>`, relative paths anchored at the root |
| `flags` | array[string] | verbatim, at both compile and link |

The three array keys are **additive**, never replacing: a profile says what is
different about it, not what the whole build is. Since every one of them lands
in the command fingerprint, editing any of them recompiles exactly what it
affects.

Two known defects, stated here rather than discovered later. `[profile.my_name]`
with an arbitrary name is **ignored in silence** — `custom` is a literal table
name, not a user-chosen one, which makes the "custom" profile of `spec.md` §13
misleading as implemented. And `lto`, `strip`, `sanitizers` and
`warnings_as_errors`, reserved by RFC-0003, are neither honoured nor rejected;
they should be rejected until they are honoured, for the reason given above
about `artifact`.

## Parallelism

Translation units have no ordering constraints, so they are compiled on a
work-stealing thread pool sized to the online CPU count. Each worker owns a
deque, pushes and pops at the bottom, and steals from the top of a randomly
chosen victim.

The mechanics matter less than two invariants:

- **No worker touches the WSDB.** Workers compile and nothing else. Freshness is
  decided before the pool starts, and results are recorded after it drains, on
  one thread. The database therefore needs no internal locking, and its
  single-writer guarantee (RFC-0004) is never tested by the build's own
  concurrency.
- **Recording runs even after a failure.** When one unit fails to compile, the
  units that succeeded are still absorbed into the WSDB. Discarding them would
  make the retry after a fixed typo recompile the whole project, which is the
  moment a developer is least willing to wait.

The pool is created and destroyed per batch, and there is no `-j`. Both are
worth fixing and neither changes the contract above.

## Implementation Status

Implemented: discovery with its sort and symlink rule, `[test].sources`, the
depfile absorption, the hybrid freshness test, whole-command fingerprints, the
mid-compile edit guard, per-profile output directories, the link line above,
test binaries in both modes, pruning of orphaned outputs, and the work-stealing
pool.

Not implemented, each waiting on something other than this RFC:

- **The global cache.** It is only worth building once there are dependencies to
  put in it, which is RFC-0008.
- **`static` and `shared` artifacts.** They need `ar`, `-fPIC` and a soname
  policy. Rejected explicitly in the meantime.
- **`molto bench`.** The `bench` profile is honoured, but no command runs
  benchmarks and `bench/` is not discovered.
- **`-j`.** The pool always takes the whole machine.
- **Arbitrarily named profiles**, and rejection of the reserved profile keys.
- **Output verification.** Objects and binaries are checked with `stat`, not
  hashed; an object corrupted in place is considered fresh. Inputs are hashed,
  which is where correctness is actually at risk.

## Non-Goals

Molto is not a build language. There are no rules, no recipes local to a
project, no shell escapes inside a build, and no way to make one target depend
on an arbitrary command. What a build does is derived from the tree and from the
manifest, and if that is not enough, the answer is a new manifest key or a
plugin — not a scripting layer.

Molto does not scan `#include` itself. Header dependencies come from the
compiler, which already resolves them exactly and for free while it works.
Parsing C is hard and parsing C++ is undecidable in the general case; a project
whose stated non-goal is being a compiler has no business attempting either
(RFC-0001).

## Reserved / Future

- Multiple binaries per package, and a library target separate from `src/`. Both
  wait on a target model, which waits on the build graph.
- Precompiled headers and unity builds. Both trade correctness of the
  incremental model for speed, and the incremental model is not the bottleneck
  yet.
- A response file for the link line, once object counts make the command length
  a real limit.
- Alternative linkers (`-fuse-ld=lld`, `mold`) as a first-class setting instead
  of a raw flag.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md) — `build`, `run`, `test` and the exit codes a failed build reports
- [RFC-0003: Project Manifest](0003-project-manifest.md) — `[target]`, `[test]`, `[env]` and `[profile.*]`, the inputs this RFC consumes
- [RFC-0004: Workspace Specification](0004-workspace-specification.md) — the WSDB that holds the freshness state used here
- [RFC-0005: Code Style](0005-code-style.md) — shares the discovery walk, widened to headers
- [RFC-0006: Analysis Result Cache](0006-analysis-result-cache.md) — replays the freshness rules specified here for `lint` and `fmt`
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — the edges this build has none of yet
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — where a dependency's include paths, libraries and defines come from

See also `spec.md` sections 6 (Philosophy), 9-10 (Artifacts and Global Cache),
12 (Incremental Compilation), 13 (Build Profiles) and 20 (Performance).
