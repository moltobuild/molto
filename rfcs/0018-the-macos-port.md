# RFC 0018: The macOS Port

- RFC Number: 0018
- Title: The macOS Port
- Status: Draft
- Created: 2026-09-04

## Summary

This RFC specifies what it means for Molto to **run on macOS**, and — like
RFC-0017 before it — scopes the work from a measurement rather than from the
word "port".

It is a delta on RFC-0017, not a replacement for it. Every rule that document
set stands here: one internal path model, no second representation, the
services are the port, and a platform is not named as supported until the suite
says so. What this document adds is the part macOS does not share with Windows,
which is smaller than the part it does and differently shaped: Windows was a
different operating system, and macOS is a different Unix.

## Motivation

`spec.md` §19 has listed macOS as a supported platform since the beginning, and
§495 records it as planned for 0.2 and again for 0.4. It has never been true.
RFC-0017 said the same thing about Windows and then made it true, and the thing
that made it possible was not a decision to care — it was a number. Nobody had
measured what the Windows port cost, so "later" was being chosen against an
unknown, and the number turned out to be small.

The same is true here, and the estimate is now better than it was for Windows,
because the Windows port did most of the work. Every place in Molto that had a
POSIX assumption baked into it now has an `#ifdef` around it and a service
underneath, and the reason is that Windows forced each one into the open. macOS
arrives at a codebase that has already been asked, once, where it is not
portable.

## What the code already is

Measured on master, 2026-09-04:

| | |
|---|---|
| `.c` files under `src/` | 84 |
| Lines under `src/` | 26,536 |
| Files carrying a `_WIN32` branch | 8 |
| `_WIN32` sites in those files | 45 |
| Tests in the suite | 863 |

Those 8 files are the platform layer, and they are the answer to "where is the
port". `process_service.c`, `fs_service.c`, `console_service.c`, `thread.c`,
`clock.c`, `viewport.c` and the two headers beside them already hold every
place Molto touches an operating system, because RFC-0017 required them to.
Nothing else in 26,536 lines has an opinion about which one it is running on.

The consequence is that macOS is a third branch in files that already have two,
rather than a search for where the branches should go. That search is what the
Windows port spent most of its time on.

## Where the cost actually is

Read out of the source before this port's CI ever ran, so the first number it
prints can be checked against an expectation. Four sites, and they are not all
the same kind of problem — which is the point of listing them by layer rather
than by file, in the shape RFC-0017's *What a compiler cannot tell you* table
established:

| Layer | What breaks | Where |
|---|---|---|
| Compile | `#include <threads.h>` — Apple's SDK ships no C11 threads | `src/util/thread.c:9` |
| Compile | `info.st_mtim` — Darwin calls it `st_mtimespec` and has no `st_mtim` | `src/services/fs_service.c:85` |
| Link | `-Wl,-soname,` — ld64 has no such option | `src/services/frontend_native.c:407` |
| Meaning | `lib%s.so.%lu.%lu.%lu` — the name is wrong in a way that links | `src/build/library.c:57` |

The first two are a compiler's to find and an afternoon's to fix.

**`<threads.h>` is the same wall mingw was.** `src/util/thread.c` exists at all
because of the Windows port: Molto reached for C11 threads directly until mingw
turned out not to ship them, and the fix was an interface — `mutex`,
`condition`, `thread`, ten functions — with the platform behind it. macOS lands
on that same interface, and Apple has the same gap for the same reason: C11
threads are optional, and neither vendor implemented them. So the POSIX branch
of that file becomes pthreads, which Linux and macOS both have and which is
what glibc implements C11 threads on top of anyway. That is one branch fewer in
the file, not one more.

**Darwin's `stat` is the Windows lesson, repeated.** The comment above
`file_written_at` records what whole-second resolution cost on Windows: not a
slow rebuild but a *skipped* one, on a tool whose job is deciding what changed.
Darwin has nanoseconds and spells the field `st_mtimespec`, so the fix is a name
and the trap is fixing it with `st_mtime` instead — which compiles, runs, and
reintroduces the exact bug that interface was written to escape.

The last two are the ones a compiler will not mention, and they are the reason
this port's CI has to run a build rather than only compile one.

**`-shared` works on macOS, and that is the problem.** clang's Darwin driver
maps `-shared` to `-dynamiclib`, so `build_link.c` needs no change and the link
succeeds. What it produces is a Mach-O dynamic library called
`libfoo.so.1.2.3`, which nothing on the platform will load by that name. The
`-Wl,-soname` beside it fails loudly, which is luck: without it the build would
be green and wrong.

**A dylib's version is not a suffix.** Linux writes `libfoo.so.1.2.3` with
`libfoo.so.1` recorded inside it and `libfoo.so` pointing at it. macOS writes
`libfoo.1.2.3.dylib`, records `libfoo.1.dylib` as the install name, and points
`libfoo.dylib` at it. The version moved to the *middle* of the name, so this is
not `.so` → `.dylib` anywhere in `library.c` — it is a third format function
beside the two that exist, and RFC-0007 already holds the reason it belongs
there and not at the call sites: what a shared library is called is not Molto's
to choose.

## What is free, and why

Everything RFC-0017 spent itself on that was about not being Unix:

- **Paths.** One separator, and it is `/`. `fs_format_path`, the long-path
  prefix, the `;` vs `:` in `PATH`, the drive-letter colon — none of it applies.
- **Processes.** `fork`, `pipe`, `dup2`, `waitpid` and the `poll` loop that
  streams a compiler's output are the POSIX branch of `process_service.c`, and
  they are what macOS has. The `CreateProcess` half was the expensive one.
- **The BSD half of POSIX.** `flock`, `mkdtemp`, `mkstemp`, `symlink`,
  `realpath`, `lstat` — several of these are BSD in origin, which is to say
  macOS is where they came from. `wsdb.c`'s single-writer lock needed
  `LockFileEx` on Windows and needs nothing here.
- **The console.** `sys/ioctl.h` and `termios.h` are real headers on macOS, so
  `viewport.c` and `conflict_prompt.c` compile as written.
- **`fnmatch.h`**, the header the Windows sweep missed and the first
  translation unit found, is present.
- **The suffix rules.** `FS_EXECUTABLE_SUFFIX` is `""`, `access(path, X_OK)`
  means what it says, and a name is a filename again.

## Rules

1. **macOS is the POSIX branch until it is not.** A third `#ifdef` is written
   only where Darwin genuinely differs from Linux, and `__APPLE__` is the
   spelling. Anywhere the two agree, they share code — a branch per platform in
   a file where two of them are identical is three things to keep correct and
   two facts.

2. **No second path model, no second name model.** RFC-0017's rule, restated
   because `library.c` is where it will be tested: the *document* says a target
   is a shared library, and the platform decides what the file is called. A
   `.dylib` in the IR would be a host's answer written into a portable
   document, which is the mistake `artifact.path` already refuses for `.exe`.

3. **The measurement gates nothing until it is green.** `macos.yml` carries
   `continue-on-error` on its steps, never on the job, for the reason RFC-0017
   gives: a run whose every job is excused reports success, and a green badge
   over a tree that does not build is worse than no badge.

4. **Apple clang is the compiler, and it is not LLVM clang.** Its version
   strings differ from upstream's, which `build_describe_compiler` reads, and
   pickup already models `apple-clang` as a vendor
   (`toolchain_service.c:68`). Molto asking pickup for a compiler on macOS is a
   separate port with a separate tally, and the CI sets `C_COMPILER` for the
   same reason the Windows job does: a red here has to be attributable.

## What proves it

The same four, and the fourth is still the strictest:

- The full test suite runs on macOS in CI, on a macOS runner, every case.
- `molto new` → `build` → `test` → `run` works end to end on a clean machine,
  with the toolchain coming from `pickup install`.
- A project with a dependency resolves, fetches and builds.
- `Molto.lock` for the same project is **byte-identical** to the one produced
  on Linux and on Windows.

Until all four hold, `spec.md` §19 overstates what is true, and the honest fix
is to say so there rather than here.

## What this port cannot borrow from Windows

**There is no cross half.** The Windows measurement asks its first question on
a Linux runner with a mingw cross toolchain — a minute against ten, and no
Windows queue. macOS has no equivalent: cross-compiling to Darwin needs the
Apple SDK, which does not travel. So `macos.yml` is one job on a macOS runner
that compiles *and* runs, and the order it asks its questions in is the order
RFC-0017's table earned: compile, link, build a program, build itself, then the
suite.

**There is no local emulator.** Windows became cheap to work on the day the
suite ran under wine on a Linux machine, turning rounds of CI into seconds. No
such thing exists for macOS on Linux, and the honest consequence is that this
port is paid for in runner minutes. That is an argument for making each run
count — measuring everything on every run rather than stopping at the first
failure — which is what the tally already does.

## Implementation Status

**2026-09-04.** The measurement ran twice on `macos-14`, Apple clang 15.0.0,
GNU Make 3.81.

The first run: **3 errors across 2 files** — `threads.h` not found, and
`st_mtim` twice. Both were predicted above and nothing else was. That is a
different result from the Windows port, whose sweep missed `fnmatch.h` and
found it in the first translation unit it compiled; 26,536 lines under `src/`
compile for Darwin with two files' worth of exceptions, which is what the
platform layer being finished looks like from the outside.

The second run, after both were fixed:

| | |
|---|---|
| `src/` compiles | 0 errors |
| The suite compiles | 0 errors |
| The binary | `Mach-O 64-bit executable arm64` |
| `molto new` → `molto run` | `Hello, world!` |
| molto builds molto | 2.08s |
| The suite | **815 passed, 44 failed, 4 skipped** |

For comparison, the first Windows run that reached its suite reported 680
passed and 163 failed.

### The 44, and why they are mostly one bug

Nearly all of them say the same sentence:

> the root of the project resolves to `/private/tmp/molto_devdeps_CVvNRR/app`,
> which is outside the workspace, the build directory, the cache and every
> dependency this build resolved

`/tmp` is a symlink to `/private/tmp` on macOS. `path_allowed`
(`src/services/ir_validate.c:263`) resolves the path under test through
`anchor` and then compares it against bounds that were never resolved, so on
Linux `/tmp/x` matches `/tmp/x` and the mismatch never appears. It is not a
test bug and it is not macOS-specific: any project under a symlinked path on
Linux is refused the same way, and macOS only makes it universal because that
one symlink is always there.

It is the containment check RFC-0014 relies on, so it gets its own change and
its own review rather than being folded into a port. Resolving both sides is
the fix; resolving neither is not, because the check exists to stop a document
naming a path outside the build.

Two failures are not that bug and are still open:
`test_process_service.c::a_capture_does_not_leak_its_pipe_into_another_child`,
and the `digest_of` failures in `test_source_service.c`.

### The tranche after that

The link and meaning layers are still unmeasured, because nothing in the suite
builds a shared library yet on this platform. `-Wl,-soname` and
`lib%s.so.%lu.%lu.%lu` are still where they were, and
`build_makes_a_shared_library_with_the_two_links_beside_it` is among the 44 —
it will be the one that says what a dylib costs.

## Non-Goals

Molto does not ship universal binaries as part of this. `arm64` is what the
port is measured on, because it is what a Mac is now; `x86_64` is not
prevented and is not promised, and RFC-0017's refusal to promise ARM64 Windows
is the same refusal for the same reason: nobody has measured it.

Molto does not learn about frameworks here. `-framework Cocoa` is a second
shape of answer for a host library, RFC-0016 §246 already reserves it, and it
is a resolver question rather than a port question.

Molto does not grow a Homebrew formula, a `.pkg`, a signed binary or a
notarised one. Distribution is a separate document; running is this one.

Molto does not target macOS versions older than what GitHub's runners offer.
A floor nobody tests is not a floor.

## Reserved / Future

- **Universal binaries** (`lipo`, or `-arch` twice), once there is a reason
  beyond symmetry.
- **Frameworks as a host-library answer**, from RFC-0016.
- **Codesigning and notarisation**, which a released binary will need before
  anyone can run it without a right-click.
- **`libtool` vs `ar`**, if Apple's archiver turns out to disagree with the one
  `library.c` drives.

## Related RFCs

| Question | Read |
|---|---|
| Every rule this document inherits | `rfcs/0017-the-windows-port.md` |
| What a shared library is called, and why the name is not Molto's to choose | `rfcs/0007-build-system.md` |
| The resolver that answers for a host library, and why it is per-platform | `rfcs/0016-host-libraries.md` |
| Why Molto asks pickup for a compiler instead of finding one | `rfcs/0003-project-manifest.md` |
