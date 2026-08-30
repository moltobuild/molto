# RFC 0017: The Windows Port

- RFC Number: 0017
- Title: The Windows Port
- Status: Draft
- Created: 2026-08-28

## Summary

This RFC specifies what it means for Molto and pickup to **run on Windows**, and
scopes the work from a measurement rather than from the word "port".

It is not about producing Windows binaries. A Linux machine can already do that
by driving a cross toolchain, and nothing in this document is needed for it.
This is about a person whose computer is a Windows computer being able to type
`molto build`.

## Motivation

`spec.md` §19 lists Windows as a supported platform and has since the beginning.
It has never been true. The 0.2 entry records that Windows and macOS were
planned and did not arrive; 0.4 records them again. They were never dropped,
only moved, and the reason each time was the same: a second platform multiplies
the cost of every feature after it, and depth on one platform was worth more
than breadth across three.

That reasoning has not changed. What has changed is that nobody had ever
measured what the port actually costs, so "later" was a decision taken against
an unknown number. This RFC exists because the number turned out to be small,
and a decision to defer should be taken against the real one.

## What the code already is

Measured across both repositories, 2026-08-28:

| | Molto | pickup |
|---|---|---|
| Source files (`.c` + `.h`) | 148 | 88 |
| Lines under `src/` | 24,333 | 9,123 |
| **Files that include a POSIX header** | **18** | **12** |
| POSIX call sites | 94 | 85 |

**Thirty files of two hundred and thirty-six.** Seven-eighths of the code never
speaks to the operating system at all, because it parses TOML, resolves a graph,
composes a command line or renders a report — none of which has a platform.

And what does speak to the OS is concentrated:

| File | POSIX calls |
|---|---|
| `pickup/src/services/process_service.c` | 44 |
| `molto/src/services/process_service.c` | 40 |
| `pickup/src/services/fs_service.c` | 18 |
| `molto/src/services/fs_service.c` | 11 |
| *the other 26 files, together* | 66 |

**Four files carry 63% of the 179 calls.** That is not luck. Both repositories
were built with a `process_service` and an `fs_service` that own the system, and
that structure is the port: the layer this RFC needs already exists and already
has every caller pointed at it.

Of the 179, about **56 have no direct Windows equivalent** — `fork`, `execv`,
`waitpid`, `pipe`, `dup2`, `flock`, `realpath`, `isatty`, `ioctl`, `symlink` —
and nearly all of them are inside those same four files.

Three things that a port usually founders on, and that are already absent:

- **Path building is centralised.** `fs_format_path` has 138 callers in Molto
  and pickup has the same function. The 116 format strings containing `/` go
  through it, and Win32 accepts `/` in any case.
- **Terminal control is five escape sequences** in two files, `progress.c` and
  `viewport.c`. Windows 10 renders them once the console mode is set.
- **Absolute paths are four**, across both repositories.

The `PATH` separator (`:` against `;`) is fifteen sites.

## Where the cost actually is

The tests, and it is not close:

| | Molto | pickup |
|---|---|---|
| Test files | 66 | 28 |
| **Assuming POSIX** | **37** | **13** |

Fifty of ninety-four. But they are not fifty problems: they are **two idioms**,
`mkdtemp("/tmp/…XXXXXX")` used 86 times and `system("rm -rf …")` used 104 times.
One temporary-directory helper and one remove-tree helper in **moltest** retire
190 uses of POSIX, and every test then reads better on Linux too.

That is the shape of the whole port. It looks like thirty-three thousand lines
and it is four service files and two test helpers.

**And this measurement was wrong about the tests, in a way worth recording.**
The two idioms were real and retiring them was easy. What it missed is that
seven test files fabricate a fake compiler by writing `#!/bin/sh` into a file
and calling `chmod` — a POSIX dependency that no grep for POSIX headers or
POSIX calls can see, because it is a string literal. Windows has no shebang for
`CreateProcess` to honour, so on the first Windows run those seven files took
about forty tests down with them.

The lesson generalises past this port: **a count of what the code calls is not a
count of what the code assumes.** The assumptions that hurt are the ones written
as data.

## What this is not

**Not cross-compilation.** Producing a `.exe` from Linux needs a toolchain that
targets Windows and nothing else; it has been demonstrated end to end and needs
none of this document. The two are independent and should not be sequenced
against each other.

**Not MSVC.** Molto on Windows is built with **clang targeting
`x86_64-w64-mingw32`**, the same toolchain family pickup can already publish.
`[target].compiler` models `msvc` as a vendor and nothing behind it; making
Molto's own code build under MSVC is a separate question with a separate answer,
and answering it is not required to run on Windows.

**Not a Windows product.** No installer, no MSI, no registry keys, no service.
Molto is a command-line tool that will be a command-line tool there too.

## The shape: the services are the port

The port is written **inside** `process_service.c` and `fs_service.c`, in both
repositories, and **their headers do not change**. Everything above them is
already portable and must stay that way.

This is the rule that the measurement earns and that the implementation must
not spend: a `#ifdef _WIN32` anywhere outside the platform files is a defect,
because the moment platform knowledge escapes those files it escapes into all
two hundred and six of the others, and the 87% that costs nothing today starts
costing something on every change.

The three pieces, in the order they matter:

- **Spawning.** `fork` + `execv` + `pipe` + `dup2` + `waitpid` becomes
  `CreateProcess` with pipe handles. It is the real work, and it is one function
  written twice.
- **The filesystem.** `realpath` → `GetFullPathNameW`, `flock` → `LockFileEx`,
  `mkdtemp`, directory walking, `stat`. Shims, and few.
- **The console.** `isatty` → `GetFileType`, the window size, and setting
  `ENABLE_VIRTUAL_TERMINAL_PROCESSING` once so the five escape sequences render.

## Rules

- **`/` stays the internal separator, everywhere, and nothing converts it.**
  Win32 accepts it. More importantly, `Molto.lock` is committed and diffed and a
  manifest is meant to be the same file on three platforms — a build that
  rewrote separators would make every lock differ by operating system, which is
  a diff nobody can read and a reproducibility claim Molto could not keep.
- **A path is long until proven short.** The 260-character limit is a real
  ceiling and the cache paths already approach it. Long-path handling — the
  `\\?\` prefix — belongs in `fs_format_path` and in no other place, which is
  the same argument as the separator rule and the same single point of change.
- **No platform knowledge above the two services.** Stated twice on purpose.
- **A failure says what failed, not what the API returned.** A `GetLastError`
  number in a message is a message a person has to go and look up; the
  diagnostics discipline of RFC-0011 applies unchanged.

## What differs for a person on Windows

Two things are genuinely different, and both are consequences of the platform
rather than choices:

**A shared library is a DLL.** RFC-0007 names a shared library
`lib<name>.so.<major>.<minor>.<patch>`, records a soname, and puts two symlinks
beside it. Windows has none of that: the artifact is `<name>.dll` with an import
library `lib<name>.dll.a`, there is no soname because the loader has no such
concept, and there are therefore **no symlinks to create** — which is fortunate,
because creating one on Windows requires privilege or Developer Mode. The
convention exists to be recognised by the rest of the system, and on Windows
the rest of the system recognises a different one. RFC-0007 will need this as an
amendment.

**Reading a symlink still works.** Discovery's symlink rule only follows links;
it never makes one, and following is unprivileged.

## The order: pickup before Molto

Molto does not choose a compiler — it asks pickup, and it is the only part of
Molto that knows pickup exists. A Molto running on Windows with no pickup
running on Windows has no toolchain, no formatter and no linter, and every
command that needs one fails.

So pickup is ported first, and it is the smaller of the two: 88 files, 12 of
them POSIX, 9,123 lines. It also proves the platform layer on the smaller
codebase before the larger one commits to it.

## What proves it

pickup's own specification states the bar better than this document could:
*"Naming a platform as supported before it is proven would be the one thing this
project exists not to do."* So the claim is not made until all of it holds:

- The full test suite runs on Windows in CI — both suites, every case, on a
  Windows runner rather than in an emulator.
- `molto new` → `build` → `test` → `run` works end to end on a clean Windows
  machine, with the toolchain coming from `pickup install`.
- A project with a dependency resolves, fetches and builds, which exercises the
  fetch path, the cache and the lock.
- `Molto.lock` for the same project is **byte-identical** to the one produced on
  Linux. This is the strictest of the four and the one worth failing on: it is
  what says the separator rule held, that no absolute path leaked, and that the
  document a build is planned from does not depend on where it was planned.

Until then `spec.md` §19 continues to overstate what is true, and the honest fix
in the meantime is to say so there rather than here.

## What a compiler cannot tell you

The single most useful thing this port taught, and the reason its CI has the
shape it does: **compiling proves almost nothing.** Every failure below appeared
one layer deeper than the last, and none of them was visible to the layer above.

| Layer | What failed | Why nothing earlier caught it |
|---|---|---|
| Compile | `realpath`, `lstat`, `st_mtim`, `mkdir`, `fork`, `pipe`, `waitpid`, the `W*` macros | — |
| Link | `nanosleep`, `clock_gettime` | mingw **declares** both and provides neither, so the compiler is satisfied |
| Discovery | every candidate is `gcc.exe`; `PATH` splits on `;` | the binary builds, starts, and reports version 0 toolchains — exit 0 |
| Meaning | the toolchain was *named* `gcc.exe`, so `c++` was chosen to compile C; the health probe linked into `/tmp` | everything ran; the answers were wrong |

Two of those deserve keeping.

**On Windows a suffix is a permission.** There is no execute bit, so
`access(path, X_OK)` succeeds for any file that exists and filters nothing. What
says a file may be run is that it is called `.exe` — which makes the suffix the
filter, and makes requiring it the Windows half of a check POSIX does with a
mode.

**A name is not a filename.** `gcc.exe` is what the filesystem holds; `gcc` is
what the program is called, and everything that ranks a driver, prefers one
spelling over another, or derives `g++` from `gcc` reads the name. Keeping the
suffix on it produced a toolchain that was neither preferred nor recognised as a
C++ driver, and so lost to a `c++` that then could not compile C. The two need
separate functions and the conversion needs to be inverse.

The order the CI asks its questions in comes straight from this table: build,
then *find a compiler*, then *answer for one*, then the suite. A job that only
built would have gone green with every one of these inside it.

## Implementation Status

**pickup builds, runs, finds a compiler and answers for one on Windows.** It
does so in CI, on a Windows runner, against the MSYS2 gcc that runner has. That
is the first three of the four bars in *What proves it*; the fourth, the suite,
is watched rather than gated: of 267 cases on master at 2026-08-30, **221 pass,
36 fail and 10 skip**, against a green Linux run of the same tree.

That tally moved from 192/57/14 by making the fake programs real programs, and
the move is worth more than its size. What remains is no longer one cause: the
36 fall across `recipe` (15), `diagnostics` (7), `archive_service` (6),
`gcc_install` (3), `install_service` (2) and three singletons. Six of them are
the colon again, one layer further out — MSYS2's `tar` reads `D:\path` as
`host:path` and answers `Cannot connect to D: resolve failed`. A drive letter
is not a hostname, and nothing in pickup's own code was wrong.

**Molto is untouched, which is the order this document argued for** — but it is
now measured, and the reading is in `.github/workflows/windows.yml` beside the
job that will check it. Its POSIX surface is the same shape as pickup's and
larger in one place:

- `fork` + `pipe` + `dup2` + `waitpid` + `poll` in `process_service.c`, which
  has to become `CreateProcess`. Bigger than pickup's was, because Molto
  streams a compiler's output through a poll loop while it runs rather than
  collecting it at the end.
- `flock` in `wsdb.c`, the single-writer lock, which is `LockFileEx`.
- `symlink()` in `build_service.c`, the two links beside a shared library.
  **This is the one that is not a substitution**: Windows symlinks need a
  privilege or Developer Mode, so the Unix convention this project adopted for
  shared libraries has no faithful Windows spelling, and choosing what replaces
  it — a copy, an import library, nothing at all — is a design decision this
  RFC still owes.
- `sys/ioctl.h` in `viewport.c` and `termios.h` in `conflict_prompt.c`, which
  are the console API.
- `realpath` seven times, plus `mkdtemp`, `mkstemp`, `lstat`, `chmod` and
  `access` — every one already answered inside pickup's `fs_service`. Molto
  copies the answers rather than inventing second ones.
- `<threads.h>`, C11 rather than pthreads, in `task_pool`, `loader` and
  `report`. Whether mingw provides it is a question for the cross job, not for
  a reading of the source — which is the lesson of *What a compiler cannot tell
  you*, applied before the fact for once.
- `<fnmatch.h>` in `style_config.c`, **which the reading above missed**. mingw
  has no such header, and the first run of the cross job hit it in the first
  file it compiled. It was missed because the sweep looked for a fixed list of
  POSIX headers and a fixed list of POSIX calls, and `fnmatch` was in neither —
  the same shape of miss as the `/tmp` inside a `#define`. Glob matching is
  needed on both platforms and belongs in a service, which is where it goes.

That first run taught something about the job as well as the port: it reported
one error and stopped, because `make` halts at the first translation unit that
fails. One error out of an unknown number is a tripwire, not a measurement. The
job runs `make -k` now and prints a tally — errors, files, and distinct kinds,
which is the figure that directs the work: eleven `realpath` sites are an
afternoon and one `fork` is a week.

**The first real measurement, on 2026-08-30**: 14 errors across 9 files under
mingw 10.3 cross-compiling, and **31 across 16** under MSYS2's gcc 16.2 on a
Windows runner — `realpath` five times, `st_mtim` four, `threads.h` three, and
one each of `mkdir`'s arity, `termios.h`, `sys/ioctl.h`, `poll.h`, `LOCK_EX`,
`LOCK_NB`, `fnmatch.h`, `symlink`, `strsignal`, `lstat`, `fmemopen` and
`flock`. The seven `-Wint-conversion` errors are not their own problem: they
are what an implicit `realpath` returning `int` does to the line that assigns
it.

**That 31 is a floor, not a total.** A missing header ends its translation unit
where it stands, so `process_service.c` reported `poll.h` and said nothing
about the `fork`, `pipe`, `dup2` and `waitpid` behind it — the largest piece of
the port is the one the count cannot see yet. The same shape as the lesson
above, one level down: a build that stops early does not merely delay the news,
it reports a smaller number with no sign that it is smaller.

And `tests/` is not in either figure. The test binary links every object under
`src/`, so nothing under `tests/` is compiled until `src/` does, and a step
that tried anyway just re-reported the source errors and doubled the total.

What the measurement got right: the platform lives in the services, and
`fs_service` and `process_service` carried nearly all of it. What it got wrong,
in both directions:

- **More services were involved than two.** `paths_service` needed one
  `#ifdef` — Windows names the home directory `USERPROFILE`, and only a shell
  that brings its own environment sets `HOME` at all. The rule held everywhere
  else: no platform knowledge reached `detect/`, `commands/` or `model/`, and
  the callers that used to spell `realpath` or split `PATH` now ask a service.
- **The duplication was worse and therefore better.** Two files had their own
  copy of the PATH-splitting loop and their own `PATH_SEPARATOR`; fixing the
  separator in one would have left the trap in the other. They share
  `fs_walk_path` now, and the port left the tree smaller than it found it.
- **The scattered details were as small as counted** — the separator, the
  console, the four absolute paths — but there were more of them than the sweep
  found, because a sweep for calls does not find a `/tmp` inside a `#define`.
- **A tool a test shells out to is part of the port.** The sweep looked for
  POSIX calls in the source and found none in `archive_service`, which spawns
  `tar`. The trap was in the argument, not the call.

**Neither tool ships a Windows binary, and neither should yet.** Both release
workflows publish one static x86-64 Linux artifact, and adding a `.exe` beside
it would name a platform as supported on the evidence of a compile. That is the
one thing *What proves it* exists to forbid.

## Non-Goals

Molto does not grow a second internal path model. One separator, one format
function, no conversion layer; the alternative is two representations of every
path and a bug wherever they meet.

Molto does not become buildable by MSVC as part of this. Running on Windows and
being compiled by every Windows compiler are different projects, and conflating
them is how the first one never ships.

Molto does not model anything Windows-specific — no registry, no services, no
COM, no manifest embedding. A tool that builds C and C++ needs a process, a
filesystem and a console, and those are what this port provides.

Molto does not promise ARM64 Windows. The platform layer will not prevent it and
nobody has measured it.

## Reserved / Future

- **macOS**, which shares nearly all of this: it is POSIX, so the two service
  files barely change. What it does need is its own answers for RFC-0016's
  resolver, for frameworks, and for `lib<name>.<version>.dylib` — a third
  shared-library convention, differing again from both of the others.
- **MSVC as a toolchain Molto can drive**, which pickup already models as a
  vendor and answers for nowhere.
- **The long-path story beyond the prefix**: whether Molto refuses a path it
  cannot make safe, or opts the process into long paths and requires a Windows
  version that supports it.

## Related RFCs

| Question | Read |
|---|---|
| The diagnostics discipline a platform failure still owes | `rfcs/0011-build-diagnostics.md` |
| What a shared library is called, and why the name is not Molto's to choose | `rfcs/0007-build-system.md` |
| The resolver that answers for a host library, and why it is per-platform | `rfcs/0016-host-libraries.md` |
| Why Molto asks pickup for a compiler instead of finding one | `rfcs/0003-project-manifest.md` |
