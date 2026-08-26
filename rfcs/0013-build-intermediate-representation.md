# RFC 0013: Build Intermediate Representation

- RFC Number: 0013
- Title: Build Intermediate Representation
- Status: Draft
- Created: 2026-08-20

## Summary

This RFC specifies the **Build Intermediate Representation**: the document that
describes what is to be built, independently of who described it. It defines the
**node types** the representation is made of, the rule that makes `Project.toml`
one **frontend** among several rather than the only path into the build, how the
document is **encoded** and **versioned**, when it is **cached** and when it is
**fingerprinted**, and — the part that matters most — what the engine **refuses
to lower** onto a command line.

It supersedes the section of RFC-0007 titled *The build graph*, which states
that Molto has none, and it is the contract RFC-0014 hands to a plugin and
RFC-0015 executes. Nothing about incremental compilation, the shape of a compile
line, the three stores or parallelism changes: this RFC sits upstream of all of
them, and every guarantee they make is a guarantee it inherits.

## Motivation

Molto knows what it is building and cannot say so.

`plan_project()` reads a manifest, walks a source tree, resolves a dependency
graph, folds every dependency's interface into a target scope, asks pickup for a
toolchain, and composes compilation passes — in one function, in one pass, with
no representation of the result that outlives the call. There is no moment at
which the question "what is going to be built, and why" has an answer that is
not a debugger. `molto metadata` comes closest, and it describes dependencies
rather than work.

That is survivable while there is exactly one way to describe a project. It
stops being survivable the moment there are two. A frontend for Meson, a
transform that generates C, a packager that turns a binary into a `.deb` — each
of them needs somewhere to put its answer, and if there is no such place then
each of them must reach into the middle of a two-thousand-line function and
agree with it about arrays of fixed-width strings. That is not an ecosystem,
it is a patch set.

RFC-0007 saw this coming and wrote down what was missing: "targets as nodes
rather than three hard-coded shapes, a topological order over package
dependencies, cycle detection, and a reverse index from header to dependent
unit". This document is that list, generalised by one step — because the nodes
are not only useful to Molto's own frontend, and a representation invented for
one producer is a representation the second producer will find wrong.

There is a second motivation, and it is the load-bearing one. A plugin is going
to hand Molto a description of work, and Molto is going to execute it on a
developer's machine with a developer's privileges. Whether that is safe has
nothing to do with how the plugin was sandboxed and everything to do with what
the description is allowed to say. A representation with no specified vocabulary
is a representation whose vocabulary is "anything", and there is no way to
review a build description that can say anything.

## The document

An IR document is a `Project` and everything reachable from it. Ten node types,
and no eleventh — a producer that needs to express something outside this list
is describing something Molto does not build, and the answer is a new node type
in a new revision of this schema rather than a free-form escape.

### `Project`

| Key | Type | Description |
|---|---|---|
| `name` | string | The package name, as `[package].name` |
| `version` | string | Semver, as `[package].version` |
| `root` | path | The directory every relative path in the document is anchored at |
| `targets` | array[`Target`] | What this project builds |
| `dependencies` | array[`Dependency`] | What it builds against |
| `origin` | string | The frontend that produced it: `native`, or a plugin name |

`origin` is not provenance for a log. It selects the lowering rules of *What the
engine refuses to lower*, below, and it is the reason a document carries it at
all.

### `Target`

A thing that is built. This is the node RFC-0007 said was missing, and its
arrival is what retires "there is exactly one executable per package".

| Key | Type | Description |
|---|---|---|
| `name` | string | Unique within the project |
| `kind` | string | `executable`, `static`, `shared`, `object` or `test` |
| `sources` | array[`Source` \| `GeneratedSource`] | The translation units |
| `options` | array[`CompileOption`] | Its own compile scope |
| `includes` | array[`IncludePath`] | Its own include scope |
| `links` | array[`LinkOption`] | What its link line names |
| `depends_on` | array[string] | Other target names in this project |
| `artifact` | `Artifact` | What it produces |

`depends_on` names targets, never files and never commands. It is the only edge
a producer may draw between two units of work, and it is what the topological
order of RFC-0015 is computed over. A cycle is an error at validation, reported
against the document, not discovered as a deadlock in the scheduler.

### `Source` and `GeneratedSource`

| Key | Type | Description |
|---|---|---|
| `path` | path | Relative to `Project.root` |
| `language` | string | `c` or `cpp` |
| `options` | array[`CompileOption`] | The unit scope of RFC-0007 |

A `GeneratedSource` carries the same three keys and two more: `produced_by`, the
name of the `BuildStep` that writes it, and `deterministic`, a boolean the
producer **MUST** set. Both are load-bearing, and RFC-0015 says why: a source
that does not exist until something ran is a source the freshness model of
RFC-0007 was not written for.

### `Dependency`

| Key | Type | Description |
|---|---|---|
| `name`, `version` | string | The resolved coordinate (RFC-0008) |
| `origin` | string | `registry`, `path`, `git` or `archive` |
| `root` | path | Where its sources or headers are on this machine |
| `interface` | table | Its `includes`, `links`, `options` — what it exports |

A dependency's interface is exactly what RFC-0009's `[artifacts]` declares, in
the vocabulary of this document rather than a second one.

### `IncludePath`, `CompileOption`, `LinkOption`

The three option nodes share a shape: a `value`, and a `scope` of `target`,
`profile` or `unit`.

The scope is not decoration. RFC-0007 fixes the order in which the three scopes
reach a compile line, states that the order is contract rather than detail, and
gives the two reasons — the order is the fingerprint, and a compiler takes the
last of two contradictory flags. Carrying the scope on the node is what lets a
transform add an option without knowing where in the line it belongs, and what
lets the engine compose the same line it composes today. **The engine orders by
scope; a producer's array order is preserved only within a scope.**

`IncludePath` carries one extra key, `system`, distinguishing `-I` from
`-isystem`. It exists because a dependency's headers and a project's headers
deserve different warning treatment, and because expressing that as a raw flag
in `CompileOption` would hide an include path from every consumer that wants to
reason about include paths.

### `Artifact`

What a target leaves behind: a `kind` mirroring the target's, a `path` relative
to the profile's build directory, and an optional `install` name.

An `Artifact` of kind `static` or `shared` is **expressible and not yet
executable**. RFC-0007 rejects a manifest that asks for either, because
producing a `.a` needs `ar` and a `.so` needs `-fPIC`, a soname and a versioning
policy, and none of those exist. That refusal stands, and it moves: the IR may
carry the node, and the engine reports that it cannot build it. Separating what
the representation can say from what the implementation can do is what keeps a
frontend for Meson — a build system whose whole vocabulary is libraries —
writable before Molto grows shared library support.

### `BuildStep`

A command that is not a compile and not a link.

| Key | Type | Description |
|---|---|---|
| `name` | string | Unique within the project |
| `program` | string | What to run |
| `args` | array[string] | Its arguments |
| `inputs` | array[path] | Files it reads |
| `outputs` | array[path] | Files it writes |
| `phase` | string | `generate` only, in this revision |

This is the node that derogates one half of RFC-0007's Non-Goals, and it should
be read with that sentence in view: "no shell escapes inside a build, and no way
to make one target depend on an arbitrary command". A `BuildStep` is a way to
make a target depend on a command, and pretending otherwise would be dishonest.

What is preserved is the reason that sentence was written. There is still no
build language: a `BuildStep` is not written by hand in a `Project.toml`, has no
conditionals, no variables, no interpolation and no shell — `program` is
executed directly and never through `sh`, so a quoted argument is an argument
and not a parse. It is a node a *frontend* emits, from a `custom_target` that
already exists in a file that already describes a build, and both its inputs and
its outputs are declared so the scheduler can order it and the freshness model
can check it. A build that can run a command is a real widening of what Molto
does; a build that can run a command **it cannot see the inputs and outputs of**
is the thing that was refused, and it is still refused.

## `Project.toml` is a frontend

The native manifest produces an IR document exactly as any other frontend does,
and the engine consumes IR exclusively. There is no second path.

The alternative was to leave the native build as it is and treat the IR as the
thing plugins speak, folding their output into the existing plan. It is less
work and it is wrong, for a reason worth stating plainly: two paths diverge, and
they diverge in the direction that punishes the newcomer. Every capability the
IR grows would have to be re-implemented by hand on the native side, every fix
applied to one would drift from the other, and a frontend for Meson would be a
second-class citizen forever — able to describe only what somebody remembered to
translate. A representation that its own author's tools do not use is a
representation nobody has tested.

Made native, the rule is symmetric: whatever `Project.toml` can express, a
frontend can express, because they are writing the same document.

## One document, two encodings

An IR document is JSON on the wire, and TOML when a human wrote it as a test
fixture. Both are the same document, and both get one reader — the argument is
already in `include/molto/util/doc.h`, made for recipes, and it holds here
unchanged: two readers for one format drift, and they drift asymmetrically,
because the copy that only ever sees a plugin's answers has no local file
anyone can diff against.

`molto ir` emits JSON only. There is no TOML writer in the tree and this RFC
does not ask for one; hand-written TOML is an input, never an output.

**The reader needs one extension it does not have.** `doc_view` deliberately
covers "the subset a recipe uses — tables of scalars and string arrays", and an
IR document is arrays of *tables*: a list of `Target`, each holding a list of
`Source`. Both backends can already express it — TOML stores table arrays and
addresses them as `name[0]`, and JSON has arrays natively — so what is missing
is an accessor over `doc_view`, not a capability in either encoding. That
accessor is the one piece of plumbing this RFC requires before anything else
can be built.

**Fixed-width buffers do not survive contact with this document.** A manifest
option is capped at ninety-five characters (RFC-0007), which is fine for a `-D`
somebody typed and is not fine for the command line of a Meson `custom_target`
or a path into the shared cache. IR nodes are heap-allocated, using the existing
`str_list`, and the caps that make sense for a hand-written manifest **MUST
NOT** be applied to a document a machine produced.

## Schema, and the node it does not know

Every document opens with an integer `schema`. This revision is `2`.

Revision `2` adds `scope` to `Dependency`. It is a revision rather than a plain
addition because the directional rule below cuts the wrong way for this one
attribute: an older reader would ignore it and read a development dependency as
a runtime one, folding it into `src/` — the exact leak RFC-0008 exists to
prevent, arrived at silently. The revision is what turns that into a refusal
that names itself.

Two parties read an IR document and they read it in opposite directions: Molto
reads what a plugin returned, and a plugin reads what Molto sent. Both will
eventually meet a document written by a newer version of the other. The rule is
directional, and it is the most important paragraph in this RFC:

> An **unknown attribute** on a known node is ignored. An **unknown node type**
> is fatal, in both directions.

RFC-0009 says unknown keys are ignored, so that a format can grow without a flag
day, and that is right for a document that *describes* something. It is wrong
for a document that *is executed*. An engine that ignores a node type it does
not know builds something other than what was described, and reports success. A
plugin that ignores a node type Molto added deletes it from the document it
returns, and Molto builds something other than what it asked for, and reports
success. Both failure modes are a green build of the wrong thing — precisely
what every other RFC in this repository is arranged to prevent.

Attributes are safe to ignore because an attribute refines work that is already
described; a node type is work that is not described at all.

A plugin's recipe therefore carries a minimum Molto version and the IR schema it
speaks (RFC-0014), so that the incompatibility is found before the process is
spawned rather than in the middle of a document.

## What the IR does not fingerprint

RFC-0007 fixes an object's fingerprint as the entire command line plus the
environment it runs in, and refuses to keep a list of "flags that matter"
because every such list is eventually wrong. That rule stands, and this document
is upstream of it.

**The IR is not part of an object's fingerprint.** A change to the
representation that does not change any command line **MUST NOT** cause a
recompile. Doing otherwise would mean that renaming a node, reordering a
producer's output, or releasing a plugin with an internal refactor invalidates
every object in `~/.molto/`'s shared cache — a store that is never pruned. It is
the same argument RFC-0011 used to keep presentation out of a command line, and
it costs more here.

**The exception is mandatory, and it is the nodes that become commands.** A
`BuildStep` fingerprints exactly like a compile: the whole program and argument
vector, plus the environment. A `GeneratedSource` is a prerequisite like any
other, and the file it produces is compared by content — never by mtime, because
a regenerated file has a new mtime every time whether or not a byte changed, and
RFC-0007's hybrid test exists precisely so that a timestamp is not evidence.

The rule generalises: **nodes that lower to a command are fingerprinted as
commands; nodes that only describe are not fingerprinted at all.**

## Where an IR is cached

In the WSDB, under `.bin/`, and never in `~/.molto/`.

RFC-0004 already reserved the space — its list of what a workspace database
holds names a build graph second — and RFC-0006 already established the shape:
a key, a fingerprint of the inputs, and a payload, discarded fail-safe when
either the format version or the fingerprint disagrees. An IR entry is the fifth
kind alongside `input`, `object`, `binary` and `result`, and it obeys the same
invariants: reached only through the Workspace API, written by one process under
`flock`, discarded rather than repaired.

It does not go in the global cache, and the reason is RFC-0007's rule that a
global store must be addressed by content rather than by path. An IR document is
full of absolute paths — the workspace root, `~/.molto/cache/sources/...` — so
two machines produce different bytes for identical work. It is also cheap to
recompute, which is the other half of RFC-0008's test for what deserves to be
kept.

A cached IR is invalidated by:

- the manifest, and **every file the frontend read**. A frontend **MUST** return
  that list with its document — a Meson frontend that reads `meson.build` and
  four `subdir()` files and reports only the first has produced a cache entry
  that is silently wrong;
- the **version of the plugin binary** that produced it. RFC-0006 makes this
  argument for clang-tidy — two releases do not find the same things, and a
  replayed diagnostic from the previous one is a lie about the current one — and
  a replayed IR from a previous frontend is a larger lie;
- the IR **schema version**;
- `[env]`, because a frontend is a process and a process reads its environment;
- the **resolved dependency graph**, since every `Dependency` node and much of
  every include and link line comes from it.

## What the engine refuses to lower

Permissions govern the plugin's process. They do not govern the document it
returns, and the document is executed by Molto, as the user, with the user's
privileges. A plugin restricted from touching the network can return an IR whose
`CompileOption` is `-fplugin=/tmp/x.so`; a plugin with no filesystem access can
return an `IncludePath` of `/`. Four of the eight capabilities of RFC-0014 —
`generator`, `command`, `compiler`, `linker` — are, by construction, ways to run
a program under a name Molto chose.

The defence is not the sandbox. It is that a document is validated before any of
it becomes a command, under rules that depend on where it came from:

**Applied to every document, whatever its origin:**

- A `path` **MUST** resolve inside the workspace root, the profile's build
  directory, the global cache, or a root the caller authorised. A path that
  escapes all four, whether by `..`, by an absolute prefix or through a symlink,
  is a rejected document.
- The authorised roots are the directories `resolve` found the build's packages
  in, and they are **supplied by the caller, never read back off the document**.
  A `[deps]` entry of `{ path = "../greet" }` is a sibling checkout — outside the
  first three and named on purpose by the person who wrote `Project.toml` — so a
  rule with only three bounds would refuse a correct project. A producer that
  could widen its own bounds by writing a `Dependency` node would be held to
  nothing, which is why the list does not come from the document. A frontend's
  answer is validated before anything has been resolved, so it is held to three.
- A `BuildStep.program` is executed directly, never through a shell, and is
  resolved as an absolute path, a workspace-relative path, or a name found in
  the toolchain — never by searching an inherited `PATH`.
- Every output of a `BuildStep` **MUST** be inside the build directory. A step
  that writes into `src/` is editing the user's code as a side effect of a
  build.
- A document **MUST** be acyclic, and is checked before execution rather than
  discovered during it.

**Applied to a document whose `Project.origin` is a plugin, and not to the
native frontend:**

- Compiler options that load code into the compiler — `-fplugin`,
  `-fplugin-arg-*`, `-load`, `-Xclang -load` — are rejected. They are a second
  extension mechanism, arrived at sideways, with none of this RFC's rules.
- Options that redirect the toolchain — `-B`, `--sysroot`, `-fuse-ld` pointing
  outside the resolved toolchain — are rejected. The toolchain is pickup's
  answer (RFC-0003) and not a frontend's opinion.
- Options that write outside the build tree, `-o` among them, are rejected: the
  engine composes output paths, and a producer that names one is describing
  where its object goes, which is not its decision.
- A `BuildStep` requires the `generator` capability, declared in the plugin's
  recipe, and is refused from a plugin that only declared `frontend` or
  `transform`.
- A `Dependency` node is refused outright. It is not a declaration of a need: it
  carries the version that was resolved, the origin it came from and the
  directory the bytes landed in on this machine, and all three are answers
  `resolve` gives. `resolve` is the phase RFC-0015 closes to plugins, because a
  plugin that could influence which versions a build uses would make a lock file
  a suggestion. A frontend describes a project; it does not describe its graph.
  The day a frontend needs to say "this project wants zlib", that is a node this
  schema does not have.

The asymmetry is deliberate and it is not a statement about trust.
`Project.toml` is a file in the user's repository, which they wrote, which
their reviewer read and their version control records. A plugin's document is
generated on the fly by a binary fetched from a registry. Those two deserve
different scrutiny even when the second is entirely well-behaved, and the day
they do not is the day the first one stopped being reviewable.

A rejected document is reported as a diagnostic against the producer, naming the
node and the rule, and exits with the plugin failure code of RFC-0014 — never as
a build failure, because the build never started.

## One project, or many

A document holds exactly one `Project` in this revision.

A document with several would be most of a multi-package workspace, which
RFC-0003 reserves as `[workspace]` and RFC-0004 explicitly defers, and arriving
at it through the back door of an IR schema would settle a design nobody
discussed — package selection, shared lock files, whether one member's profile
binds another. Frontends that describe several projects, and Meson is one,
produce one document per project and Molto composes them, which is exactly the
dependency edge it already has.

The array is a single-element array rather than a scalar, so the day
`[workspace]` is specified the schema widens without a revision.

## `molto ir`

`molto ir` writes the document for the current project to standard output, or
to `--output <path>`. It runs the frontend and every transform, and stops before
the engine.

It exists because a contract that cannot be inspected is a contract nobody can
conform to, and because the whole of this RFC is untestable without it: a
frontend is tested by comparing the document it produces against a fixture.

It carries `molto metadata`'s rule, for the same reason that command gives: no
timestamp, no serial, no absolute path that a second machine would write
differently, and two runs over one project **MUST** produce one byte-identical
file. A dump that differs between runs cannot be diffed, and a document that
cannot be diffed cannot be reviewed or cached.

## Implementation Status

The document exists and the engine reads half of it. As of molto 0.21.0:

- **The table-array accessor** is `doc_array_len` / `doc_array_at` / `doc_table_at`
  in `include/molto/util/doc.h`. Reaching it needed a fix in the TOML parser
  first: `[[targets.sources]]` was stored flat under `targets.sources` and lost
  which element it belonged to, so every target's sources merged into one list.
  A nested header is now qualified with its ancestors' indices.
- **The node types and their validation** are `ir_service.h`, `ir_service.c`,
  `ir_read.c` and `ir_validate.c` — eight of the ten. `BuildStep` and
  `GeneratedSource` are refused by name, with the reason, rather than silently
  absent.
- **The native frontend** is `frontend_native.c`, and `molto ir` prints what it
  produces. It describes the executable and the tests: `[test].mode` decides
  whether the suite is one target per file or one for all of them, and each test
  target `depends_on` the executable. That edge does not say "minus the entry
  point" — two `main()` do not link, so the engine drops the executable's own
  because a linker would refuse the alternative, which is a law rather than a
  policy. When `Target` of kind `object` arrives with RFC-0015's graph, the
  library objects become a target of their own that both depend on, and the law
  stops needing to be applied.
- **`molto ir`** is implemented, `--output` and `--profile` included, and its
  output is byte-identical between runs.
- **Transforms** are implemented, and `merge_deps()` is one of them. Its own
  comment made the argument before transforms existed: folding a dependency's
  interface into the target scope means "none of those has to learn what a
  dependency is". `ir_transform_dependencies` says what `resolve` found and
  `ir_transform_fold_dependencies` folds it in.
- **`scope` on `Dependency`** is carried, in revision `2`. The fold takes a
  document and nothing else: the node says `runtime` or `dev`, so a consumer
  holding only the published bytes folds them exactly as the engine does. It is
  required rather than defaulted, because a missing scope and a runtime scope
  would otherwise be the same document and only one of them is safe.
- **`package` on `Target`** is carried. A target that names one has its paths
  relative to that `Dependency`'s root rather than to `Project.root`, which is
  how a package whose bytes live in the shared cache is described without
  putting a machine's home directory in the document. Naming a package the
  document does not describe is refused rather than falling back to the project
  root, and the bounds check is unchanged: the anchor moves, the fence does not.
- **The path rule reaches the native document.** `plan_project` validates what
  the transforms produced, against the four bounds, before any of it becomes a
  command. It is what turns a dependency's recipe — which a remote party wrote —
  from something that could name any directory on the consumer's compile line
  into something held to the same rule as everything else.
- **A dependency's own sources are targets.** One `Target` of kind `object` per
  package that ships sources, named `<package>:objects`, carrying that package's
  own recipe and nothing the consumer resolved. Everything a build compiles now
  has its command line read off the document, and `build_compile_argv` has one
  branch instead of two.

Ordered by what blocks what:

- **The table-array accessor on `doc_view`**, without which no document of this
  shape can be read at all.
- **The node types and their validation**, including the acyclicity check and
  the lowering rules above.
- **The native frontend**: `Project.toml` and the discovery walk producing a
  document instead of a plan.
- **The engine reading a document**, with `units_from()` as the seam — it
  already turns sources and options into compile units.
- **`Target` as a real node**, which is what retires one-executable-per-package
  and the target model RFC-0007 reserved.
- **`BuildStep` and `GeneratedSource`**, which wait on RFC-0015 deciding what
  `generate` does to the progress denominator and the freshness model.
- **WSDB caching of documents**, last, because a cache for something that is not
  yet slow is a cache that is only a bug surface.

## Non-Goals

The IR is not a build language. It has no conditionals, no variables, no
interpolation, no functions and no way to express "if". Every question of that
kind is answered by the producer, at the time the document is made, and what
reaches Molto is the answer rather than the question. The place where a
condition may be evaluated is a frontend's own input file — a `meson.build` has
`if`, and interpreting it is the frontend's job (RFC-0014) — and the document
that comes out the far side is flat.

The IR is not a serialisation of Molto's internal structures. It is a specified
document that happens to be what Molto uses, and a change to a struct that is
not a change to this schema is not a change to the IR. The distinction is the
whole reason for choosing a document over an ABI.

The IR is not a distribution format. It is not published, not fetched, not
addressed by a coordinate and not stored in a registry: it is derived from a
project, on a machine, at a moment, and it is full of that machine's paths.

The IR does not describe how to compile. It says what a translation unit is and
what options apply to it; the order those options reach a command line, the
driver chosen, the profile flags and the link composition are RFC-0007's, and
this document deliberately says none of it twice.

## Reserved / Future

- **Several `Project` nodes per document**, when `[workspace]` is specified.
- **A `phase` on `BuildStep` beyond `generate`** — a step after link is what a
  packager needs, and RFC-0015 has nowhere to display one yet.
- **Content-addressed documents**, which would make an IR shareable between
  machines and turn `molto ir` into the input of a remote build. It needs every
  absolute path replaced by a root-relative one plus a named root, which is a
  larger change than it looks.
- **A reverse index from header to dependent unit**, which RFC-0007 names as a
  thing a real graph will need once object counts reach the thousands.
- **Feature nodes**, if `[features]` (RFC-0003) is specified. Conditional
  compilation is the one thing a producer cannot pre-answer, because the
  condition is chosen by the consumer.

## Related RFCs

- [RFC-0003: Project Manifest](0003-project-manifest.md) — the input the native frontend reads
- [RFC-0004: Workspace Specification](0004-workspace-specification.md) — the WSDB a document is cached in, which already reserved room for a build graph
- [RFC-0006: Analysis Result Cache](0006-analysis-result-cache.md) — the key/fingerprint/payload shape an IR entry borrows, and the tool-version invalidation rule
- [RFC-0007: Build System](0007-build-system.md) — supersedes *The build graph*; everything else it specifies is downstream of this document and unchanged
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — where every `Dependency` node comes from
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — `[artifacts]`, which is a dependency's interface in another vocabulary
- [RFC-0014: Plugin System](0014-plugin-system.md) — who else produces these documents, and under what permissions
- [RFC-0015: Build Pipeline and Transforms](0015-build-pipeline.md) — what executes one

See also `spec.md` sections 6 (Philosophy), 12 (Incremental Compilation) and 14
(Plugins).
