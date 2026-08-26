# RFC 0015: Build Pipeline and Transforms

- RFC Number: 0015
- Title: Build Pipeline and Transforms
- Status: Draft
- Created: 2026-08-20

## Summary

This RFC specifies the **pipeline**: the six phases a build passes through, what
each one may change, why a **transform** is the mechanism a plugin should reach
for and a **hook** is not, why the **scheduler** belongs to Molto and cannot be
lent out, and what `generate` — the one phase that can create work out of
nothing — costs the two guarantees the build already makes.

It executes what RFC-0013 describes and runs what RFC-0014 installs. It changes
nothing about how a translation unit is compiled: RFC-0007 owns the compile
line, the freshness rules and the thread pool, and this document is the ordering
around them.

## Motivation

RFC-0007 describes a build as two things happening in sequence: compile
everything, then link. That is an accurate description of a build with no edges,
and it stops describing anything the moment a source can be generated, a target
can depend on a target, or a third party can insert work.

The gap is not "Molto needs more phases". It is that the phases already exist,
unnamed, inside one function, and so there is nowhere to say when a plugin runs.
"Before the compile" is not a specification while there is no noun for the thing
that happens before the compile.

There is a second and more urgent motivation. Two of Molto's guarantees are
stated in absolute terms and both assume that the set of work is known before
the work starts. RFC-0007: "A build is planned in full before any of it runs."
RFC-0012: the progress denominator exists before the first compiler is spawned.
A generated source violates both by definition — it is a translation unit that
does not exist until a command has run. Either the guarantees are qualified
deliberately, in writing, or they will be violated accidentally, in code, and
the symptom will be a progress bar that lies and an incremental build that
occasionally keeps a stale object. This RFC exists mostly to make that choice on
purpose.

## The phases

```
resolve ─▸ generate ─▸ transform ─▸ compile ─▸ link ─▸ package
```

| Phase | Input | Output | May a plugin act |
|---|---|---|---|
| `resolve` | manifest, lock file | dependency graph | no |
| `generate` | IR | files, and IR nodes for them | `generator` |
| `transform` | IR | IR | `transform`, `target` |
| `compile` | IR | objects | `compiler` |
| `link` | objects | binaries | `linker` |
| `package` | binaries | distribution artifacts | `packager` |

The frontend runs before all six: it is what produced the document, and by the
time the pipeline starts there is an IR (RFC-0013).

`resolve` is closed to plugins, and that is a deliberate refusal rather than an
omission. Resolution decides which versions a build uses, writes a lock file and
verifies checksums; a plugin that could influence it would make a lock file a
suggestion, and the whole argument of RFC-0008 rests on it not being one.

`generate` and `package` are optional and, in a project with neither generated
sources nor a packager, absent. `transform` runs at least once for every build,
because folding a dependency's interface into the target scope is itself a
transform.

## Transforms compose; hooks do not

A **transform** is a function from an IR document to an IR document. It is
given the whole document, returns a whole document, and Molto composes several
by running them in sequence.

A **hook** is a callback at a named moment — `before_compile`, `after_link` —
that acts by side effect.

Both can express the same work, and they are not equally good, for a reason that
has nothing to do with taste. Two transforms compose into a transform: their
composition has the same type as its parts, so the order is a list, the result
is inspectable at every step, and "run A then B" needs no coordination between A
and B. Two hooks on one point do not compose into anything. They both fire, in
an order somebody has to define, mutating shared state, and whether the result
is correct depends on what each did to what the other was expecting. The first
model is a pipeline; the second is a plugin ecosystem where the bug reports are
about interactions.

Transforms are also the reason the model is testable. A transform's test is a
document in and a document out, comparable byte for byte with `molto ir` — a
hook's test is a build, and an assertion about a side effect.

**A transform is pure with respect to the filesystem.** It reads the document
and returns a document. It does not write files: a transform that needs a file
to exist emits a `BuildStep` and lets `generate` run it, which is what makes the
work visible, orderable and cacheable instead of a surprise that happened during
planning.

**Hooks exist, and they are narrow.** A plugin may register at `before_compile`
and `after_link`, and those two points are it. They are for the things a
transform genuinely cannot express — observing a build, reporting on it,
producing a summary after a link that is not itself an artifact — and they
**MUST NOT** modify the IR. A hook that could change the document would be a
transform that ran at a moment when the document has already been lowered to
commands, which is how a build ends up doing something no document described.

The precedence is: express it as a transform; if it produces a file, emit a
`BuildStep`; if it produces neither, it is a hook, and it may only observe.

## The scheduler belongs to Molto

Plugins contribute work. They do not order it, and they do not run it.

The dependency edges are in the document — `Target.depends_on`, and a
`BuildStep`'s inputs and outputs — and Molto computes the topological order,
checks for cycles before anything runs, and dispatches into its own thread pool.
A plugin never receives a callback that means "now do your part in parallel",
and never learns how many workers there are.

The two invariants of RFC-0007 are unchanged and are the reason:

- **No worker touches the WSDB.** Freshness state is written by the coordinating
  thread. A plugin process, which is not even in the same address space, is not
  an exception to a rule the build's own threads obey.
- **Recording runs even after a failure.** Work that completed before the
  failure is recorded, so a re-run does not redo it. A plugin that scheduled its
  own work would have to reimplement that, and would get it wrong in the case
  that only shows up after a failed build.

This is also what keeps parallelism a property of Molto rather than a
negotiation. Independent compiles run at once because the graph says they are
independent, and a `BuildStep` runs concurrently with anything it does not
share an input or output with — decided by the document, not by a plugin's
opinion of its own parallelism. It is also why a recipe's `jobs` key stops
making sense for anything Molto schedules (RFC-0009): handing a parallelism flag
to a subordinate build system is a delegation, and there is nothing to delegate
to when the work is in the graph.

## `generate` is a barrier

`generate` runs every `BuildStep` to completion before planning begins. Nothing
compiles while a step is running, and no step runs after the first compile has
started.

This is a strong rule, it costs real parallelism, and it is chosen over the two
alternatives because of what they cost instead.

### What the barrier buys

**A progress denominator that is true.** RFC-0012 requires the total to exist
before the first compiler starts, and RFC-0007 states that a build is planned in
full before any of it runs. Neither is decoration: the viewport's region is
sized from the count, and a count that grows mid-build either redraws a region
that has scrolled or reports a percentage that goes backwards. With the barrier,
every generated source exists by the time planning happens, so the plan is
complete and the count is final — the existing guarantees hold word for word,
with no qualification.

**An incremental model that still applies.** RFC-0007's freshness rules assume a
prerequisite is a file a *user* edited. Two of them break on a file this build
produced. The guard that refuses to record an object whose source moved during
its own compile would fire constantly on a generated header if generation and
compilation overlapped — it would be detecting a real race, and the race would
be Molto's own. And `-MMD` reports a generated header as a prerequisite like any
other, with a fresh mtime after every regeneration, which is precisely why the
freshness test hashes content when the timestamp disagrees. With the barrier,
generated files are finished and stable before anything reads them, and the
freshness model needs no special case.

### What the barrier costs

**A generator cannot depend on a compiled artifact of the same project.** A tool
built by this project cannot generate this project's sources — no `moc` compiled
here and run here, no code generator built from `src/` and used by `src/` in one
pass.

That is a real limitation and it is stated rather than discovered. The intended
answer is the one the ecosystem already has: a generator is a `tool` in the
registry, or a workspace member built first, or a `[build-deps]` entry once that
is specified. Building your own tool inside your own build in one invocation is
a genuinely hard problem — it needs a host/target split, two toolchains and two
graphs — and taking it on before targets, shared libraries or cross-compilation
exist would be building the roof first.

### What a generator must guarantee

> A `generator` **MUST** be deterministic: identical inputs produce
> byte-identical outputs.

This is not a style rule. A generator that stamps a timestamp into a header
produces a new file on every run, the file is a prerequisite of everything that
includes it, and every build recompiles everything downstream forever. The
freshness model's content hash is what saves a build from a timestamp, and it
only saves it if the content actually matches.

A step whose outputs are unchanged since its last run is skipped, on the same
terms as an object: the command is fingerprinted whole — program, arguments and
environment — and its inputs are compared by the same hybrid mtime-then-hash
test. A generated file that is rewritten with identical bytes does not
invalidate anything downstream, because the content decides.

## Where a plugin's work is shown

RFC-0012 specifies what a build looks like while it runs, and a plugin's work
has to fit inside it rather than beside it.

**The plugin supplies a name; Molto composes the line.** A row is built by one
function so that the label and the row cannot drift apart, and truncation is
correctness rather than tidiness — a row wider than the terminal destroys the
anchor the whole region is drawn from. A plugin therefore hands over a name and
never preformatted text, exactly as a compile unit does.

**A generated step needs an origin.** The build report classifies each unit by
where it came from — a registry package, a path or git dependency, the project's
own sources, a test — and a `BuildStep` from a plugin is none of the four. It
gets a fifth, so the viewport can name it as what it is instead of borrowing a
word that is wrong.

**A packager has nowhere to appear, and this RFC does not invent one.**
RFC-0012 does not name the link step because by then the region has been torn
down; `package` is after the link, which puts it after the viewport exists at
all. The honest options are to extend the region past the link or to let
`package` print plainly like everything else that happens after a build. This
RFC takes the second — a packager reports through ordinary output — and marks
the first as the thing to revisit when there is a real packager to look at. A
progress model designed for a workload nobody has run yet is a model designed
wrong.

## The cut in the code

The pipeline exists in the tree today, unnamed, inside `plan_project()` in
`src/services/build_service.c`. The line between describing and executing falls
inside that function, immediately before `toolchain_resolve()`:

| Today | Becomes |
|---|---|
| `load_project()` | the native frontend |
| the source discovery walk | `Source` nodes |
| `prepare_and_lock()` | the `resolve` phase |
| `merge_deps()` | a transform |
| `compose_include_flags()` | a transform |
| the C++ scan | an attribute of the document |
| `toolchain_resolve()` | the engine, and everything after it |

`merge_deps()` is worth naming because it makes the argument for transforms
better than this RFC can. Its own comment explains that a dependency's include
directories, defines, flags and libraries are exactly the things `[target]`
already carries, so folding them together means nothing downstream has to learn
what a dependency is. That is a transform: one function, one document in, one
document out, and every consumer simplified by it having run.

On the other side of the line, nothing changes and nothing is extensible.
`plan_pass()`, `unit_argv()` and `build_link_argv()` compose commands, and they
are where RFC-0007's ordering contract lives. `units_from()` is the seam — it
already turns sources and options into compile units, which is exactly the
lowering the engine performs on a document.

`build_plan` holds both halves today: dependencies, sources and test sources on
one side, passes and counts on the other. It splits in two — a document, which
is serialisable and is what `molto ir` prints, and a plan, which is derived,
never serialised, and never seen by a plugin.

## Implementation Status

Nothing here is implemented as a pipeline, and much of it is implemented as
straight-line code. `resolve`, `compile` and `link` exist and work;
`transform` exists as two function calls that are not called that; `generate`
and `package` do not exist at all.

The split of `plan_project()` has started. `load_project()` and the source
discovery walk are gone from it: it asks the native frontend for a document and
`document_sources()` lowers the targets it describes into the units the passes
compile. `Target` is therefore load-bearing — a test target's `depends_on` is
what a test binary links against — and `build_plan` now holds the document it is
derived from. The line has not moved past that: everything the compile line says
still comes from `project_ctx`.

The options half is done for everything the project owns. `build_compile_argv`
composes a unit's line by scope from the document — target, then profile, then
unit, the order RFC-0013 fixes — and `merge_deps` and `compose_include_flags`
are transforms rather than two hard-coded calls. The fold is now a transform in
full: IR schema 2 puts a `scope` on `Dependency`, so it takes a document and
returns a document, and a consumer holding only the published bytes reproduces
it exactly. Two consequences were chosen rather than discovered: a scope reaches
the line as its options and then its includes, because a document cannot tell a
define from a flag, which reorders `-D -I -F` into `-D -F -I` and misses the
shared object cache once; and `-std` is a unit-scope option, so `[target].std`
now wins over one written by hand into `flags`.

The options half is now done for everything, the dependencies included. A
package is a `Target` of kind `object` carrying its own recipe, so
`build_compile_argv` reads one document and has no second branch, and
`compile_unit` no longer carries the manifest's answer to the same question. The
same reorder applies to a dependency's line as to the project's: `-std` is a
unit-scope option and reaches it last, which misses the shared object cache
once per package.

The link line is read off the document too, as of IR schema 3. A `LinkOption` is
what reaches the linker, so `build_link_argv` pushes one loop over the node's
`links` by scope and never asks whether a value is a library or a flag. Two
consequences, both chosen: `-o` moves ahead of them so the scopes stay
contiguous, and a library sits with its scope rather than at the end of the
line — every link is the same multiset, reordered, and a binary is relinked once.
It also retired the three lists `build_tests` used to widen by hand for
`[dev-deps]`: the fold puts them on targets of kind `test` and nothing else.

Not implemented:

- **A topological order over targets**, which needs `Target` to be a node
  (RFC-0013). Today the only ordering in a build is "all objects, then the
  link".
- **Cycle detection**, which is validation of a document and belongs with it.
- **`generate`**, the barrier, and step freshness.
- **Transforms as a registered, ordered list**, rather than two hard-coded
  calls.
- **Hooks**, which are last on purpose: they are the least useful half of this
  document and the easiest to regret.
- **A fifth build origin** for the viewport.

## Non-Goals

The pipeline is not configurable. The phases are fixed, their order is fixed,
and there is no manifest key that adds one, removes one or reorders them. A
build whose shape depends on the project is a build language, which RFC-0007
refuses and RFC-0013 declines to represent.

A plugin does not schedule. It cannot request a worker, cannot ask for its work
to run first, and cannot mark a step as needing exclusive access. Where those
turn out to be needed they are properties of the document — an edge — not
privileges of a process.

`package` does not define distribution formats. What a `.deb` is belongs to a
packager plugin; this RFC says only when it runs and what it is given.

This RFC does not change the compile line, the link line, profiles, the
freshness rules or the thread pool. All five are RFC-0007's, all five are
unchanged, and where this document mentions one it is to state that it survives.

## Reserved / Future

- **A `generate` phase that is not a barrier**, once a host/target split exists
  and a project can build a tool for itself. It needs two toolchains and two
  graphs, and it is the natural home of the limitation stated above.
- **A packager inside the viewport**, if a real packager shows that plain output
  after the build is not enough.
- **Post-link steps** — stripping, signing, compressing — which are `BuildStep`
  nodes with a phase that does not exist yet (RFC-0013).
- **Remote execution.** The scheduler's ownership of the graph is what makes it
  conceivable: a document with content-addressed paths could be dispatched
  somewhere other than this machine without a plugin noticing.
- **Per-step resource limits**, once anything in a real build is slow enough to
  need them.

## Related RFCs

- [RFC-0004: Workspace Specification](0004-workspace-specification.md) — the WSDB the coordinating thread writes freshness into
- [RFC-0007: Build System](0007-build-system.md) — the compile line, the freshness rules and the thread pool this pipeline orders, all unchanged
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — the `resolve` phase, closed to plugins
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — `[build].jobs`, which has nothing to delegate to when the work is in the graph
- [RFC-0011: Build Diagnostics](0011-build-diagnostics.md) — how a failing step reports
- [RFC-0012: The Build Viewport](0012-build-viewport.md) — the denominator the barrier protects, and the origin a step needs
- [RFC-0013: Build Intermediate Representation](0013-build-intermediate-representation.md) — the document this pipeline executes
- [RFC-0014: Plugin System](0014-plugin-system.md) — the capabilities that map onto these phases

See also `spec.md` sections 12 (Incremental Compilation) and 20 (Performance).
