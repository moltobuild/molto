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

## Implementation Status

Nothing. This RFC specifies and implements none of it.

The measurement above is the only work done: it establishes that the port is
four service files, two test helpers and a set of scattered details that can be
counted on two hands, rather than an unbounded rewrite. It does not establish
that the decision to defer was wrong — only what the decision costs.

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
