# RFC 0006: Analysis Result Cache

- RFC Number: 0006
- Title: Analysis Result Cache
- Status: Draft
- Created: 2026-08-04

## Summary

This RFC specifies how Molto stores **what a tool said about a file**, so that
`molto lint` and `molto fmt` skip work that has already been done. It adds one
entry kind to the WSDB (RFC-0004) and fixes the rules that decide when a
recorded result may be replayed instead of recomputed.

It does not introduce the idea of caching analysis: RFC-0005 already specifies
it under *Caching*, and lists the per-file cache as not implemented for one
stated reason — the WSDB has nowhere to put a tool's output. This RFC is that
missing half.

## Motivation

`molto build` is incremental. `molto lint` is not, and both read the same
inputs.

On this repository the difference is invisible — 74 files, 0.18s either way. It
is not invisible anywhere else. The compiler's syntax-only pass is the cheap
half of a lint run; `clang-tidy` is the expensive one, by design, because it
builds an AST and walks it once per enabled check. A thousand-file project pays
that on every invocation, which puts `molto lint` outside a pre-commit hook and
inside the set of commands people run once a week, while the `molto build` next
to it stays instant. A linter that is too slow to run is a linter that does not
run.

Everything needed to know whether a file must be re-analysed already exists in
the WSDB, put there by the build: the file's freshness signature, the headers it
included, and the exact command. What is missing is somewhere to record the
answer.

## Why the obvious cache is wrong

The cheap version of this feature stores a boolean: this file was clean. It is
worse than no cache at all.

A file with two warnings is not clean, so it is re-analysed every time and
nothing is saved where saving matters. And a file that *is* clean under a
configuration where warnings do not fail the command still has diagnostics to
print. `molto lint` would report them on the first run and print nothing on the
second, with the same exit code both times. In CI, where the first run is on a
cold cache and the second is not, that is a green build that hid its warnings.

The rule this RFC is built on:

> **A cached run must be indistinguishable from an uncached one, except in how
> long it took.**

Same diagnostics, same order, same counts, same exit code, byte for byte. A
cache that cannot promise that is not permitted to exist, because the failure it
produces is silent and the thing it silences is the tool's entire output.

## The entry

RFC-0004 gives the WSDB three entry kinds — input, object, binary — and this
adds a fourth: **result**.

A result entry records what the analysis of one file produced:

- **Key**: the file. This RFC first said the pass and the file, on the reasoning
  that the compiler's syntax pass and the linter's pass are different tools that
  should be invalidated independently. Implementing it showed why they cannot
  be: the prerequisites come from the dependency list the *compiler* pass
  writes, so deciding whether the linter's entry is still valid means running
  the compiler pass anyway. Keeping one entry per file costs a syntax-only run
  when only `linter.json` changed — the cheap half of a lint — and buys an entry
  that cannot disagree with itself about which headers it watched.
- **Fingerprint**: what the answer depended on. Identical to the one the build
  already computes for an object — the exact command line, the file's freshness
  signature, and the headers absorbed from the depfile — extended by the two
  things that speak only to analysis: the resolved tool's version, and the
  translated backend configuration under `.bin/style/`.
- **Payload**: every diagnostic the tool produced, normalized as RFC-0005
  describes, in the order it produced them.

**A file with no diagnostics is recorded, with an empty payload.** "Analysed and
had nothing to say" and "never analysed" are different facts and the store must
not conflate them; conflating them is how the boolean cache above gets built by
accident.

## Invalidation

A result is recomputed when any part of its fingerprint changes:

- the file's content — by mtime and size, confirmed by hash, as everything else
  in the WSDB;
- any header it included, through the same depfile graph the build already
  absorbs;
- the command — which covers the profile, since `--profile` decides the defines
  and a `#ifdef` decides what is even compiled;
- the translated configuration — so editing `linter.json` or `format.json`,
  changing `preset`, or excluding a path invalidates exactly what it should;
- the tool's version. Two releases of `clang-tidy` do not find the same things,
  and a replayed diagnostic from the previous one is a lie about the current
  one;
- the `[env]` the tools run in. They are launched with the project's variables,
  so a diagnostic recorded under one environment does not answer for another.

This last point differs deliberately from how RFC-0004 treats compilers, where
installing a newer one does **not** invalidate the recorded toolchain, because
changing compiler is an explicit act with consequences for the artifacts. Here
there are no artifacts: the whole content of the entry is what a specific
version of a specific tool said. When that version changes the entry is not
stale, it is about something else.

`molto clean --all` removes `.bin/` and therefore the cache. `prune` covers
result entries the same way it covers orphaned objects: a file that no longer
exists takes its results with it.

## Escaping the cache

`--refresh-analysis` re-runs every file and rewrites the entries, for the case
this design cannot cover: a tool that is not deterministic, or one whose
behaviour depends on something outside the fingerprint. It is the same shape as
`--refresh-toolchain` and `--refresh-tools`, which re-ask pickup rather than
reuse its recorded answer, and it is named for what it refreshes because the
three are otherwise easy to confuse.

Nothing about it should be needed in normal use. A flag that users learn to pass
by default is a cache that is not trusted, and an untrusted cache should be
fixed or removed rather than worked around.

## Concurrency

Results are produced by the existing task pool, one file per task, and written
on the main thread where the order of files is still the order they were
discovered in. The WSDB keeps its single-writer `flock`: parallelism is in
running the tools, never in the store.

## Correctness before speed

The obligation of the previous section is testable, and this RFC requires that
it be tested rather than assumed:

- a run on a cold cache and a run on a warm one produce identical output and
  identical exit codes, for a project with warnings, with errors, and with
  neither;
- touching a header re-analyses the files that include it, and only those;
- editing `linter.json` re-analyses everything;
- a tool version change re-analyses everything;
- a file with no diagnostics is not re-analysed on the second run.

The last one is the only test about performance, and it is expressed as a fact
about the store rather than as a measured time, because a timing test that runs
on a busy machine fails for reasons that have nothing to do with the cache.

## Implementation Status

**Implemented for `molto lint`.** The store is the `result` entry kind in the
WSDB, which the compiler pass feeds by writing its dependency list to
`.bin/lint/`. A run that changes nothing replays every file and spawns no
process at all: on this repository, 1.37s cold against 0.003s warm, with stdout
and stderr identical byte for byte.

Two things are recorded conservatively. A pass that crashed, was killed, exited
without saying anything, or had its output truncated is **not** recorded, since
storing it would replay a failure that a second attempt might not have and never
retry it. And a file whose dependency list could not be read is analysed again
next time, silently: the result is unaffected, only the time, and a line per
file would be noise proportional to the project.

**Implemented for `molto fmt`**, with one difference the entry did not need to
change for. A formatted file depends on nothing but itself: `clang-format` reads
the file it is given and none of its includes, so the prerequisite list is the
file alone and no dependency list is written.

Its key does **not** name the mode. What is recorded is that a file is already
in its final form under this style, and `--check`, `--diff` and a write are
three ways of asking the same question — so formatting a project leaves the
cache warm for the check that follows it in CI. An entry is written when that
is true: after a write, which has just made it so, or after a check or diff that
found nothing to change.

The saving is real and much smaller than lint's, because `clang-format` is not
`clang-tidy`: 0.062s to 0.003s over this repository, against 1.37s to 0.003s.

## Non-Goals

- **A shared or remote cache.** The WSDB is one workspace's incremental state,
  on one machine. Distributing it is a different problem with a different
  threat model.
- **Caching across profiles as one entry.** The profile is in the command and
  therefore in the fingerprint; `debug` and `release` results coexist as
  separate entries rather than invalidating each other.
- **Partial re-analysis within a file.** The unit is the file, as it is for the
  compiler.
- **Storing formatted content.** `molto fmt` caches the *result* of formatting —
  whether the file changed, and the diagnostics — not the formatted text. The
  file on disk is already that.

## Reserved / Future

- Extending the same store to `molto build`'s compiler diagnostics, so warnings
  survive an incremental build instead of vanishing with the objects that were
  not recompiled. This is the same problem and would reuse the entry unchanged;
  it is left out here because it changes what a build prints, and that belongs
  in its own RFC.
- A size bound and an eviction policy, if result payloads make the WSDB large
  enough to matter. They do not today, and a limit invented before the number is
  known is a limit invented wrong.

## Related RFCs

- [RFC-0002: CLI Specification](0002-cli-specification.md) — `molto lint`, `molto fmt`, exit codes
- [RFC-0004: Workspace Specification](0004-workspace-specification.md) — the WSDB, its entry kinds and invariants
- [RFC-0005: Code Style](0005-code-style.md) — the caching this implements, and the diagnostics model it replays
- [RFC-0007: Build System](0007-build-system.md) — the freshness rules this reuses, put in the WSDB by the build
