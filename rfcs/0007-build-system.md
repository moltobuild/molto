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
  machines for identical inputs, and reproducible builds are a stated goal —
  see *Reproducibility* below for what that does and does not promise.

`[test].sources` (RFC-0003) extends the test set with extra directories or
individual files, for suites whose sources live outside `tests/`. A directory is
walked; a file is taken as it is; a relative path is anchored at the workspace
root. **An entry that does not exist is a manifest error**, not a warning: the
manifest asserted that a path is part of the project, and a build that quietly
compiles less than it was told to is worse than one that stops.

## Reproducibility

Reproducibility is invoked twice above as a reason — for the sorted walk, and
for the object list that becomes the link line — so it is worth stating what it
covers, because there are two different promises under the word and Molto makes
only one of them.

**The same tree, manifest and resolved toolchain produce the same command
lines, in the same order, and the same link line.** Everything that decides a
command is either in the tree, in the manifest, in the WSDB's record of the
toolchain (RFC-0004), or in the IR a frontend produced from the first two
(RFC-0013); the walk is sorted; and no part of the composition consults the
clock, the environment, the current directory, or the order a filesystem hands
back its entries.

The fourth source is the one that needs care. A document produced by a plugin
is not a file anyone can diff, so the promise holds only as far as the producer
is deterministic — which is why RFC-0013 requires a frontend to report every
file it read, and RFC-0015 requires a generator to be byte-reproducible. A
non-deterministic producer does not break this build; it breaks this
guarantee, and it is the producer's defect.

**Byte-identical objects across machines are not guaranteed**, and three things
break them, none of which is Molto's to fix by default. `__DATE__` and
`__TIME__` bake the clock into the object. `__FILE__` and the debug information
bake in the path of the source, which differs between checkouts — Molto does
not emit `-ffile-prefix-map` or `-fdebug-prefix-map`, and does not set
`SOURCE_DATE_EPOCH`. And the compiler may embed a build id or a path into its
own installation, which is its business and not the caller's.

The distinction is not pedantry, because the two promises serve different
things. The first is what makes an incremental build correct and a shared
object cache possible: both ask "would this compile the same way", never "did
this produce the same bytes". The second is what auditing a released binary
needs, and it is a much larger commitment — it constrains which flags Molto is
allowed to pass at all. That is a decision for when there is something to
audit.

## The build graph

**Superseded by [RFC-0013](0013-build-intermediate-representation.md).**

This section used to say that Molto has no build graph, and that a flat list of
independent translation units was a sufficient description because a single
package has no edges to order. That was true while a package built exactly one
executable from one directory. It stopped being true when a target could depend
on a target and a source could be generated, and the list this section closed
with — targets as nodes, a topological order, cycle detection — is now
RFC-0013's node types and RFC-0015's scheduler.

What is unchanged, and belongs here rather than there, is where a per-object
prerequisite list comes from. It is not a graph; it is how one edge is learned.

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

One item on that old list is still open and is not RFC-0013's: a reverse index
from header to dependent unit. Freshness is answered today by walking every
object's prerequisites, which is fine for hundreds of units and will not be fine
for thousands.

## Incremental compilation

An object is **fresh**, and its compile is skipped, when all four hold:

1. the WSDB has an object entry for it;
2. the recorded command string is byte-identical to the command this build would
   issue;
3. the object file can be `stat`-ed;
4. every recorded prerequisite is unchanged.

The command fingerprint is **the entire command line**, joined by spaces, and
after it **the environment that line runs in**. There is no list of "flags that
matter", because every such list is eventually wrong: it omits the one flag
someone adds next. Recording the whole line means a changed `-D`, a changed
`-std`, a changed optimisation level, a changed include path, and a changed
compiler driver all invalidate the object by construction, without anyone having
enumerated them.

`[env]` is recorded for that same reason and not as an afterthought: the
variables reach the compiler, and `CPATH` or `SOURCE_DATE_EPOCH` change the
object as surely as a flag would. It is written after a byte no manifest can
produce, and everything past that byte is compared and hashed exactly as it
stands — an environment value is not an argument molto composed, so it may
contain spaces, and splitting it could make two environments look alike to the
shared object cache. The variables are sorted by name before they get there, so
the order two lines were written in never reaches a fingerprint. A project with
no `[env]` fingerprints byte for byte as it did before any of this existed,
which is what keeps the databases and cached objects already on disk valid.

The link fingerprint follows the same rule, and for a reason worth stating:
`[env]` reaches the linker too, so a changed `LIBRARY_PATH` is a changed binary.
Leaving it to the "something was recompiled" flag would be correct only by
accident, since every object could have come from the shared cache.

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

## The compile line

One function composes every compile line in the build, in this order:

```
<driver> -c <source> -o <object> -O<n> [-g] [-std=…]
         <defines…> <include…> <flags…>     ← target scope
         <defines…> <include…> <flags…>     ← profile scope
         <defines…> <include…> <flags…>     ← unit scope
         -MMD -MF <object>.d
         -I<src>  <-I per dependency…>
```

The order is contract rather than detail, for two reasons. It is the
fingerprint, so two compositions that differ only in their order recompile
everything and produce the same objects. And it decides which of two
contradictory flags wins, because a compiler takes the last one it is handed.

Within a scope the three keys are always `defines`, then `include`, then
`flags`. `flags` is last because it is the escape hatch, and an escape hatch
that could not override the structured keys would not be one.

The scopes are:

- **target** — `[target].defines/include/flags`, plus the interface of every
  dependency (RFC-0008). A dependency's defines and a manifest's are the same
  kind of statement, so they are folded together here rather than kept as a
  fourth scope nothing else would know about.
- **profile** — the selected `[profile.<name>]`.
- **unit** — one translation unit's own scope: `[test].options` when the unit
  is a test, and the package's own options when the unit belongs to a
  dependency.

Include paths that are relative are anchored at the project root; absolute ones
are passed through. The `-I` flags at the end are composed apart from the scopes
above and always come last, because a manifest option is capped at ninety-five
characters (RFC-0003) and a path into the shared cache is not.

Two defines are always present and appear in no manifest: `MOLTO_PKG_NAME` and
`MOLTO_PKG_VERSION`, injected into the target scope from `[package]`. They are
what lets a source report its own version without a generated header, and they
are why the target scope holds two slots more than a manifest is allowed to
fill.

### A dependency's line is not the project's

Dependencies are compiled in a pass of their own, with a deliberately smaller
line:

| | the project's sources | a dependency's sources |
|---|---|---|
| `-O`, `-g` | from the manifest and the profile | the same |
| `-std` | `[target].std` / `.cpp_std` | its recipe's, or the consumer's when it names none |
| target scope | `[target]` and every dependency's interface | empty |
| profile scope | the selected profile | empty |
| unit scope | `[test].options`, for a test | that package's own options |
| `-I` | `src/`, then every dependency's | that package's own |

The consumer's `[target]` is absent from a dependency's line for two reasons. It
would hand a library defines nobody wrote it for; and it would put the
application's `src/` on the library's include path, where a dependency's
`#include "config.h"` finds the application's. It also means a package compiles
identically in every project that depends on it, which is what makes one
compiled object worth putting in a shared cache at all.

What "that package's own options" contains is specified by RFC-0008: its private
table, its own interface, and the interface of every package it reaches — never
a sibling's. The standard is the one setting a dependency can take from the
consumer and override, per language, by naming its own (RFC-0009); everything
else is either the consumer's or the package's, with nothing to arbitrate.

All of them are in one pass regardless, because a pass is a thread pool and a
barrier, and there is no reason for two dependencies to wait on each other.

## Linking

Objects are linked with the compiler driver, not with `ld` directly, so the
driver supplies the C or C++ runtime and the startup files. The driver is the
C++ one when **any** translation unit going into the binary is C++, and the C
one otherwise; if C++ is needed and none was resolved, the build stops and says
so rather than linking with a driver that will fail obscurely.

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
per package **as implemented**; the limit is the engine's and no longer the
model's, since RFC-0013 makes a target a node and a project may describe
several.

RFC-0003 defines `[package].artifact` as `source`, `static` or `shared`. Molto
**rejects the manifest** that sets it, with any value, rather than accepting the
key and ignoring it. Producing a `.a` requires `ar`, and a `.so` requires
`-fPIC`, a soname and a versioning policy; none of that exists. An explicit
refusal costs the user one error message, while silent acceptance costs them a
debugging session over a shared library that was never built.

The refusal moves rather than relaxes. An IR document may carry an `Artifact`
of either kind — a frontend for a build system whose whole vocabulary is
libraries has to be able to say so — and the engine reports that it cannot
build it. What a representation can express and what an implementation can
produce are two questions, and conflating them would mean no frontend could be
written until shared libraries were.

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

`-j <n>` caps the worker count; without it the pool takes the whole machine,
which is the default because a build is what the developer is waiting for. The
flag is a cap on this command and nothing else: it is not recorded, it does not
reach the fingerprint, and two builds that differ only in `-j` produce the same
objects. That is what makes it safe to hand to a laptop on battery or to a CI
runner sharing a host, and what keeps it out of the manifest — how much of a
machine to use is a property of the machine, and the manifest describes the
code.

`build`, `run`, `test`, `lint` and `fmt` all take it, because all five run the
same pool over a file at a time. A cap the compiler obeys and the linter
ignores would be a cap in name only.

The pool is still created and destroyed per batch. That is worth fixing and
does not change the contract above.

## What a build says

A build is planned in full before any of it runs. Every pass — the runtime
dependencies, `src/`, the development dependencies, `tests/` — is walked first
to work out object paths, command lines and which units are stale; only then
does the first compiler start. The split costs one traversal and buys the only
number worth printing: how many units this build is going to compile.

That number is why the progress it draws is a bar and not the spinner RFC-0008
settled for. A resolution cannot say how many questions it will ask until it
stops asking; a build counts its work before doing any.

What it prints is the work, and only the work:

- one line per dependency package with at least one stale source — a package
  with forty of them is one piece of work,
- one line per source of the project's own, because that is the granularity of
  an edit,
- and one line counting everything that was already up to date or came out of
  the shared object cache. A hundred lines saying nothing happened is not a
  report.

Everything goes to stderr, beside the diagnostics it is interleaved with. The
bar and the colour are for a person at a terminal: a stream that is not one
gets the same lines with no bar and no escape sequence, and `NO_COLOR` speaks
for the person when the terminal cannot. A build that failed prints no summary
line at all — the compiler has already said the thing worth reading, and a tick
under it would be the report contradicting it.

The compiler's own stderr was once inherited rather than captured, so a
diagnostic could land on the line the bar occupied and stay there until the next
frame repaired it. It is captured now, and every block a build prints goes
through the report's lock in one act (RFC-0011). Nothing lands on the bar.

## The compilation database

A build writes `compile_commands.json` at the project root, in the format the
Clang tooling defines: one entry per translation unit, with `directory`,
`file`, `output` and `arguments`.

It exists because every tool that parses this code without being the build has
the same problem the build already solved. clangd — and behind it VS Code,
neovim, Emacs, Helix and Zed — cannot resolve an `#include` without the search
path, and cannot read an `#ifdef` without the defines. Neither can clang-tidy,
cppcheck or include-what-you-use. Molto knows both exactly, because it composed
the command line, and a project whose editor disagrees with its build about
what compiles is a project where every diagnostic has to be double-checked
against `molto build`.

Four decisions worth stating:

- **Every unit, not every compilation.** The database is filled where freshness
  is decided, not where the compiler runs, so a unit that was already up to
  date is described exactly like one that was rebuilt. A database that only
  covered what changed would be empty on the second build.
- **The arguments are what is executed, minus the depfile flags.** Nothing is
  rewritten to look tidier: `directory` is the project root and `file` and
  `output` are relative to it, which is presentation, but no argument is
  reworded, reordered or made prettier. The one omission is `-MMD -MF <path>`,
  and it is an omission rather than an edit — those two say nothing about the
  translation. They exist so the build learns which headers a unit read. Clang's
  own tooling strips them before parsing, and a tool that runs the line instead
  of reading it, like include-what-you-use, would write a depfile into `build/`
  on Molto's behalf. `-o` stays: the object a unit produces is part of what the
  compilation is, which is why `output` names it too.

  The line that is *executed* and the line that is *fingerprinted* both keep
  them. Only the description drops them, so nothing about freshness moves —
  dropping them from the fingerprint would change the hash of every unit in
  every project already on disk, and buy a full rebuild for a cosmetic gain.
- **The last command wins.** The file is written by whichever command last
  compiled, with the profile it compiled — so `molto test` leaves a database
  covering `tests/` as well, and `molto build --profile release` leaves one
  describing release. One file cannot hold two profiles, and choosing silently
  between them would be worse than following what the developer last ran.
- **It is written even when the build fails.** A command line does not become
  wrong because the code it describes does not compile, and a broken build is
  when an editor that understands the project is worth the most.

Failing to write it is a warning, never a failure: nothing about the artifact
depends on it. It is generated output — `molto new` puts it in `.gitignore`,
and a committed one would carry one developer's absolute paths into everyone
else's checkout.

## Diagnostics

**The build asks the compiler for no warnings.** `-Wall`, `-Wextra` and
`-Wpedantic` reach a compile line only when a manifest put them in
`[target].flags` or a profile's; Molto adds none of its own, and no profile
turns any on.

`molto lint` is where the opinion lives. Its `molto` preset passes exactly
those three (RFC-0005), over the same sources, through the same discovery walk.
The split is deliberate: a build's job is to produce the binary the manifest
describes, and a build that fails on a warning nobody asked for makes the
manifest a lie about what it builds. A warning is a review, it belongs to the
command whose whole purpose is reviewing, and keeping it there is what lets the
`lint` preset tighten over time without breaking builds that never asked.

The consequence worth stating: `build` and `lint` do not issue the same command
line, so a project that never runs `lint` is not seeing what `lint` would say.

### The order of what a build prints

**Diagnostics are ordered by unit.** Each compiler's output is captured whole
and printed once its unit is done, in one act against the report — which is
what `lint` and `fmt` already did under the same pool, and what the build now
does too (RFC-0011). Two units failing at once no longer interleave line by
line.

They are not ordered by *plan*: blocks arrive as their units finish, so the
first block printed is not necessarily the first unit that was queued. Buffering
every block to replay them in plan order would be fully deterministic at the
cost of saying nothing until the build ended, and that trade is not made here.

## Implementation Status

Implemented: discovery with its sort and symlink rule, `[test].sources`, the
depfile absorption, the hybrid freshness test, whole-command fingerprints, the
mid-compile edit guard, per-profile output directories, the compile and link
lines above with their scopes, test binaries in both modes, pruning of orphaned
outputs, the work-stealing pool with `-j`, and the compilation database.

Not implemented, each waiting on something other than this RFC:

- **The global cache.** It is only worth building once there are dependencies to
  put in it, which is RFC-0008.
- **`static` and `shared` artifacts.** They need `ar`, `-fPIC` and a soname
  policy. Rejected explicitly in the meantime.
- **`molto bench`.** The `bench` profile is honoured, but no command runs
  benchmarks and `bench/` is not discovered.
- **Arbitrarily named profiles**, and rejection of the reserved profile keys.
- **Output verification.** Objects and binaries are checked with `stat`, not
  hashed; an object corrupted in place is considered fresh. Inputs are hashed,
  which is where correctness is actually at risk.
- **Warnings on a cached unit.** A unit found up to date is not compiled, so
  what the compiler said about it last time is not said again. RFC-0006's store
  already records and replays that for `lint`; the build does not read it.
- **The IR.** A build is planned straight from the manifest into compilation
  passes, in one function, with no representation of the plan that outlives the
  call. RFC-0013 specifies the document and RFC-0015 the phases; until they are
  built, this RFC describes both the frontend and the engine at once, which is
  why it reads as though the two were one thing.

## Non-Goals

Molto is not a build language. There are no rules, no recipes local to a
project, no shell escapes inside a build, and nothing in a `Project.toml` that
makes a target depend on a command someone typed. What a build does is derived
from the tree and from the manifest, and if that is not enough, the answer is a
new manifest key or a plugin — not a scripting layer.

That sentence named a plugin as the escape hatch before anyone had specified
one, and RFC-0014 now has. It is worth being exact about what the answer turned
out to be, because half of this Non-Goal was given up to get it. A build *can*
now run a command: a `BuildStep` (RFC-0013), emitted by a frontend from a
`custom_target` that already existed in a file that already described a build.
What was not given up is why the refusal was there. A `BuildStep` declares its
inputs and its outputs, so the scheduler can order it and the freshness model
can check it; it is executed directly and never through a shell; it is validated
against the workspace before it runs; and no project can introduce one by
writing it in its own manifest. The thing refused was never "a command" — it was
a command Molto cannot see the edges of, and that is still refused.

Molto does not scan `#include` itself. Header dependencies come from the
compiler, which already resolves them exactly and for free while it works.
Parsing C is hard and parsing C++ is undecidable in the general case; a project
whose stated non-goal is being a compiler has no business attempting either
(RFC-0001).

## Reserved / Future

- Multiple binaries per package, and a library target separate from `src/`. The
  target model they waited on is specified — it is RFC-0013's `Target` — so what
  remains is the engine building more than one of them, plus `ar` and the
  shared-library policy above.
- Precompiled headers and unity builds. Both trade correctness of the
  incremental model for speed, and the incremental model is not the bottleneck
  yet.
- A response file for the link line, once object counts make the command length
  a real limit.
- Alternative linkers (`-fuse-ld=lld`, `mold`) as a first-class setting instead
  of a raw flag.
- **Subtracting a flag**, rather than only adding one — Kbuild's
  `CFLAGS_REMOVE_<object>`, for the file that cannot tolerate something it
  inherits. The three array keys are additive and nothing removes anything
  today. The position is reserved here rather than left open because the
  cascade has a precedence, and introducing subtraction later without a place
  already kept for it would change that precedence for every manifest already
  written.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md) — `build`, `run`, `test` and the exit codes a failed build reports
- [RFC-0003: Project Manifest](0003-project-manifest.md) — `[target]`, `[test]`, `[env]` and `[profile.*]`, the inputs this RFC consumes
- [RFC-0004: Workspace Specification](0004-workspace-specification.md) — the WSDB that holds the freshness state used here
- [RFC-0005: Code Style](0005-code-style.md) — shares the discovery walk, widened to headers
- [RFC-0006: Analysis Result Cache](0006-analysis-result-cache.md) — replays the freshness rules specified here for `lint` and `fmt`
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — the edges this build has none of yet
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — where a dependency's include paths, libraries and defines come from
- [RFC-0011: Build Diagnostics](0011-build-diagnostics.md) — what a build prints when a unit does not compile, and why no flag is asked for
- [RFC-0013: Build Intermediate Representation](0013-build-intermediate-representation.md) — supersedes *The build graph*, and the document everything specified here is downstream of
- [RFC-0015: Build Pipeline and Transforms](0015-build-pipeline.md) — the phases around the compile and link this RFC composes

See also `spec.md` sections 6 (Philosophy), 9-10 (Artifacts and Global Cache),
12 (Incremental Compilation), 13 (Build Profiles) and 20 (Performance).
