# RFC 0011: Build Diagnostics

- RFC Number: 0011
- Title: Build Diagnostics
- Status: Draft
- Created: 2026-08-17

## Summary

This RFC defines how Molto presents what a compiler and a linker say about the
code they were given: the output is **captured** rather than inherited, and each
unit's findings are **drawn in a frame** carrying the source line, a caret under
the character the tool named, and a footer saying whose code it was and what was
asked to build it.

It closes three defects RFC-0007 recorded as open — diagnostics that interleave
between workers, diagnostics that tear the progress bar, and a failure that
names an absolute path without saying which dependency it belonged to. It
supersedes the sentence in RFC-0002 that says compiler errors are surfaced
verbatim, and it extends RFC-0005 with a third output shape for `molto lint`
without moving either of the two that already exist.

## Motivation

Before this, a unit that did not compile produced one line:

```
molto: failed to compile '/home/u/.molto/cache/sources/database/1.2.0/x86_64-linux/src/database.c'
```

preceded by whatever the compiler had written directly to the build's inherited
stderr. Three things were wrong with that, and all three were consequences of
not capturing.

**The output of two failures interleaved.** Units compile concurrently and each
compiler wrote as it went, so two units failing at once produced their lines
mixed together — and the more cores a machine had, the more reliably.

**Diagnostics tore the progress bar.** The bar is drawn on one line and redrawn
every fifty milliseconds; a compiler writing beside it did not pass through the
report's lock, so its output landed on the line the bar occupied and stayed
broken until the next tick.

**The line said nothing about ownership.** A path under `~/.molto/cache/sources/`
identifies a file and not a package. Which dependency it was, at which version,
and which compiler had been asked to build it were all knowable and none of them
were said.

## Capture

`molto build`, `molto run` and `molto test` capture each compile and each link
with `process_capture_all`, into one buffer per worker held only while that
child runs. What comes back is parsed, framed, and handed to the report in a
single call — which is what makes a block atomic against the bar and against
the other workers.

Two consequences follow and are accepted.

**The live stream is gone.** A translation unit that takes a minute writes
nothing until it finishes. The bar is still ticking, which is what the bar is
for.

**There is a limit.** Sixty-four kilobytes of one tool's opinion of one file.
Everything gcc has to say about a thoroughly broken unit fits many times over;
a unit that says more than this is told so, in a note of Molto's own.

## No flag is asked for

A compiler can be told not to draw its own caret — `-fno-diagnostics-show-caret`
in gcc, `-fno-caret-diagnostics` in clang, and neither in clang-tidy. Molto asks
for none of them, and recognises a caret line on the page instead.

The reason is not portability, though the two spellings would already need
dispatching on a vendor that is empty whenever `C_COMPILER` chose the compiler
by hand. The reason is that **a compile line is a fingerprint**. Every token of
it decides whether an object is stale (RFC-0007) and is hashed into the key of
the shared object cache (RFC-0008). Adding a token would make every cached
object on the machine stale at once, and orphan every entry in
`~/.molto/cache/objects` — which is never pruned — in order to change how a
message looks. The same token in `lint`'s compile line would invalidate every
analysis result the RFC-0006 store holds.

So the rule is: **presentation never reaches a command line.** A caret line is
made of nothing but the marks, the spaces before them, and the gutter gcc puts
them in, which makes it recognisable without asking anyone. The line above a
caret is the excerpt it belongs to, and goes with it.

This also means `molto lint --format text` and `--format json` come out byte for
byte as they did: the heuristic lives in the renderer, and the parser and the
normalized writers are untouched.

## The frame

```
   ✗ Failed to compile `database.c`

   error[-Wincompatible-pointer-types]: compilation failed

   ┌─ modules/database/src/database.c:42:17
   │
42 │     return user.name;
   │                ^ incompatible pointer types
   │
   = dependency: database v1.2.0
   = source: modules/database
   = compiler: clang 20.1.2

error: build failed
```

**One header per unit**, carrying the verdict in its glyph before any of the
words: `✗` for a unit that failed, `⚠` for one that compiled and had something
to say. A build that warns is reported exactly like one that fails, minus the
`✗` and the closing verdict — capturing a warning and then not printing it
would make every warning in every green build disappear.

**One summary line and one frame per finding.** The bracket carries the
compiler's own rule where it named one and is omitted where it did not. There
is deliberately **no Molto error-code space**: clang publishes no stable codes,
gcc publishes only its flags, and a code Molto invented would be a second
identity for a diagnostic that already has one. The text after the colon states
what it meant for the build; the message under the caret is the tool's, word for
word.

**One footer per unit.** `= dependency:` names the package and version, absent
for a project's own code. `= source:` appears only when the package is somewhere
the reader can go and look — a `path` dependency inside the project — because a
cache path says no more than the coordinate above it already did. `= compiler:`
is the vendor and version pickup resolved, falling back to the name of the
binary when `C_COMPILER` chose one and nothing asked it what it was.

**`error: build failed`** closes a failed build, flush left, once, however many
units broke. It is printed for `exit_build_failure` and for nothing else: a
manifest that would not parse or a registry that would not answer has already
been reported in its own words, and nothing was built, so nothing failed to
build.

## Columns

gcc counts the column it reports the way a terminal counts: a tab advances to
the next stop of eight, and a character outside ASCII is one column however many
bytes it took. clang counts bytes. Both are defensible, they disagree on any
line holding a tab or anything outside ASCII, and a caret placed under the wrong
model lands where the compiler was not pointing.

Molto asks the resolved toolchain which it is dealing with, renders the excerpt
with its tabs advanced to the stops a terminal uses, and converts a byte offset
into a column when it has one. A vendor it cannot identify is taken to count
like gcc.

The conversion is a character count and not a display width: a character a
terminal draws two columns wide is counted as one. Doing better means `wcwidth`,
which means a locale, which Molto does not set.

## Nothing is thrown away

- A line the parser could not read is printed as the tool wrote it.
- Past ten frames for one unit, the rest fall back to the normalized one-line
  form of RFC-0005, which still says everything the frame would have. A missing
  semicolon in C cascades into dozens of errors, and forty frames is the same
  information spread over a screenful nobody scrolls back through.
- A source that cannot be read costs the excerpt and nothing else: the locator
  and the message stay. This is the ordinary outcome for `<command line>`, for
  `<built-in>`, and for anything generated and since removed — not an edge case.
- Escape sequences a project asked the compiler for, with
  `-fdiagnostics-color=always` in its flags, are taken back out: capturing
  through a pipe already turns colour off, so anything that arrives coloured was
  asked for explicitly, and it fights a frame that colours itself.

## Linking

A failing linker has a grammar of its own. It names no severity, because it has
only the one to name; it prefixes some of its lines with its own program name
and not others; and it names a place in anyone's source **only when the objects
carry debug information** — so `debug` and `bench` point at a line and `release`
quotes what the linker said instead. Its object-file context line names a
temporary nobody can open, and its driver's closing summary repeats what the
lines above already said; both are dropped.

A linker that succeeded and still spoke was warning, and is parsed as an
ordinary tool would be. Saying otherwise would fail a build that stands.

## `molto lint`

`--format` gains `rich`: the same frames, one block per file, written to stdout
beside `text` and `json`. Absent, it resolves to `rich` at a terminal and `text`
anywhere else, so a redirected lint keeps the output every script already
parses. `text` and `json` are unchanged in shape.

Analysis is not a build: nothing was produced and nothing was stopped, so a
finding states its severity and its rule and no outcome.

## What this does not do

**A cached unit does not repeat its warnings.** The compiler was not run, so
there is nothing to repeat. RFC-0006's store already records what a tool said
about a file and replays it for `lint`; extending that to the build would fix
this, and is not done here.

**Diagnostics are ordered by unit, not by plan.** Each block is atomic, and
blocks arrive as their units finish. Buffering every block to print them in plan
order would be fully deterministic at the cost of saying nothing until the end.

**A line longer than 1024 bytes is truncated**, tail dropped, by the parser this
builds on. A C++ template instantiation error exceeds that routinely, and a
frame makes the cut visible where a raw stream did not.

## Related RFCs

- [RFC-0002: CLI Specification](0002-cli-specification.md) — its "surfaced
  verbatim" is superseded here: output is captured and reframed.
- [RFC-0005: Code Style](0005-code-style.md) — owns the normalized one-line
  form, which `rich` sits beside rather than replaces.
- [RFC-0006: Analysis Result Cache](0006-analysis-result-cache.md) — the store
  that would let a cached unit repeat its warnings.
- [RFC-0007: Build System](0007-build-system.md) — the fingerprint that makes a
  compile flag expensive, and the three defects this closes.
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — the shared
  object cache whose key a compile flag would change.
