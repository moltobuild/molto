# RFC 0020: Isolated Tests

- RFC Number: 0020
- Title: Isolated Tests
- Status: Draft
- Created: 2026-10-04

## Summary

This RFC lets a test file name the sources it replaces. Molto links that file
into an executable of its own, without those sources, so a fake defined by the
test can stand in for the real function. The rest of the suite keeps its mode.

```toml
[[test.isolated]]
file     = "tests/services/test_build_failures.c"
replaces = ["src/services/process_service.c"]
```

Molto decides what is linked. It does not generate fakes or know how they are
written: moltest-mock is the framework the ecosystem ships for that.

## Motivation

A fake function in C is a definition the linker takes instead of the real one.
fff, moltest-mock and hand-written stubs all work that way, and all of them have
the same precondition: the real definition is not in the binary. Two strong
definitions of one function do not link.

Under Molto that precondition never holds. A test executable links every object
of `src/` (minus the app's `main.c`) and every object of every dependency, which
Molto compiles from source (RFC-0008). In `single` mode, which a framework that
owns `main()` requires, there is one such executable for the whole suite. So:

- a fake of a function in `src/` fails with a duplicate symbol;
- a fake of a function a `[deps]` entry defines fails the same way — the case
  moltest-mock's README promised would work (moltest-mock KI-3);
- a fake of a libc function links, but replaces it for every test in the binary,
  and, on Linux, for libc's own internal calls too.

What remains is faking functions nothing defines, which is not what a test
suite needs. The paths that need a fake are the ones that cannot be reached for
real: a compiler that crashes, a disk that fills, a registry that times out.
Molto's own suite tests those with real processes and directories, or does not
test them, and the coverage floor (RFC-0019) shows where.

No macro can fix this from inside a test framework: one binary cannot hold both
the real function and the fake. Only the build system chooses what is linked.

## Design

### The declaration

`[[test.isolated]]` is an array of tables under `[test]` (RFC-0003):

| Key        | Type          | Required | Description                                              |
|------------|---------------|----------|----------------------------------------------------------|
| `file`     | string        | yes      | A test source under `tests/`, as a path from the root.   |
| `replaces` | array[string] | yes      | Sources left out of that file's executable. Not empty.   |

An entry of `replaces` is either:

- a path to a source of the project, such as `"src/services/process_service.c"`;
- `"<dep>:<path>"`, a source of a `[deps]` entry as its recipe lists it, such as
  `"clocklib:src/clocklib.c"`;
- `"<dep>"` alone, every source of that dependency.

Each one has to name a source the test link would otherwise contain. A typo, a
moved file or a development dependency is a manifest error, not a silent no-op:
a replacement that replaces nothing leaves the real function in place and the
fake colliding with it, which is exactly the failure this RFC removes.

The same `file` may not appear twice. A file listed here is still discovered
from `tests/` as usual; the entry changes how it is linked, not whether.

### The executable

An isolated file gets its own executable, `build/<profile>/tests/<stem>`, named
by the same stem `per_file` uses. It links, in order:

1. the file's own object;
2. `[test].sources` and the development dependencies' objects, loose — that is
   where a framework's `main()` and its constructors live, and a constructor in
   an archive member nothing references is never pulled in;
3. the objects of `src/` (minus `main.c`) and of the dependencies, **minus the
   replaced sources, as a static archive**.

The archive is the point of step 3. Linked loose, every remaining object is in
the binary, and each one that calls into a replaced source needs that function
faked, whether or not the test reaches it. Linked as an archive, the linker
pulls only the members the test transitively reaches, so the test fakes what
its code under test calls and nothing else. GNU ld resolves archive members in
one pass, so Molto wraps the archive in `--start-group`/`--end-group`; ld64 and
lld do not need it.

In `single` mode the file is taken out of the shared executable. In `per_file`
mode it already has one; only step 3 changes. Either way `molto test` runs every
executable it built and reports each, as it does for `per_file` today, and what
follows `--` reaches all of them.

The archive is built per isolated file, under `build/<profile>/tests/`, from
objects that already exist: nothing is compiled twice. The replaced sources are
part of that executable's link fingerprint (RFC-0007), so editing `replaces`
re-links it and nothing else.

### When the link fails

Leaving out a source leaves out everything it defines. When the test reaches a
function of a replaced source that it does not fake, the linker reports an
undefined symbol. Molto adds what the linker cannot know (RFC-0011):

```
error: linking failed: tests/services/test_build_failures.c
  │ undefined symbol: process_capture
  = note: src/services/process_service.c defines it, and this test replaces that file
  = help: fake process_capture in the test, or stop replacing the file
```

### What it does not do

- **Replace part of a source.** The unit is a file, as it is for the linker. A
  call between two functions of one `.c` cannot be faked, here or with fff:
  keeping the object keeps the real function, dropping it drops the caller too,
  and the compiler may resolve or inline the call without the linker at all.

  ```c
  /* src/config.c: load_config's call to read_file cannot be faked */
  int read_file(const char *path) { ... }
  int load_config(const char *path) { return read_file(path) != 0 ? -1 : 0; }
  ```

  Moving `read_file` to `src/fs.c` makes the call cross a boundary, and an
  entry with `replaces = ["src/fs.c"]` can then fake it while `load_config`,
  still in `src/config.c`, is the code under test. The same holds for a
  dependency: replacing one of its sources drops every function in it, so a
  test that wants one of them real and another faked cannot have both; it
  fakes every function of that source the code under test reaches.
- **Replace a development dependency.** The test framework is not code under test.
- **Find the replacements itself.** Molto could read the test object's symbols
  and leave out whatever collides. That needs a reader for ELF, Mach-O and COFF,
  and it would make a typo in a fake's name silently link the real function.
  Saying it in the manifest is one line and is checked.
- **Generate fakes.** Writing them is the test framework's job.

## Molto testing itself

The first isolated test is the one the coverage floor points at: build failures
that need a compiler to misbehave. `tests/services/test_build_failures.c`
replaces `src/services/process_service.c` and fakes `process_run` with
moltest-mock, so it can answer "exit 1", "killed by a signal" and "not found"
without a compiler that does any of them.

Not every system call can be replaced this way. Fourteen sources under `src/`
call libc directly (`fopen`, `rename`, `mkdir`, …) instead of going through
`fs_service`; a test that needs one of those to fail first moves the call
behind the service. That is done when a test needs it, not ahead of time.

## Alternatives considered

**Weak symbols.** Production code marks a function mockable, a macro that is
`__attribute__((weak))` only when compiling tests, and the fake wins. It covers
calls inside one file, which this RFC does not, but it annotates production
code function by function, compiles `src/` differently for tests, and weak
symbols are unreliable on MinGW (RFC-0017). moltest-mock's ADR 0002 rejected it
for the same reasons.

**`--wrap`.** GNU ld redirects calls to `__wrap_f` and keeps `__real_f`. ld64
and MSVC have no equivalent.

**A declaration in the test source.** A comment such as
`/* molto: replaces src/... */` keeps the replacement next to the fakes. It also
makes Molto read C sources for a directive, which nothing else does, and puts a
build decision where `molto metadata` and the IR (RFC-0013) cannot see it.

**A mode.** `mode = "isolated"`, every test file in its own executable with
replacements per file, is `per_file` plus this RFC, and it would take `single`
away from the suite that needs it for everything else.

## Unresolved questions

- **Coverage across executables.** Several executables write counters for the
  same objects, and gcov merges them, so the measurement is right. But
  moltest-coverage reports, and applies its floor, at the end of each run: the
  first executable to finish would judge a partial measurement. Either
  moltest-coverage learns to defer to the last run, or Molto tells the test
  binaries which run is last.
- Whether `molto test -k`-style selection should be able to name an isolated
  executable, or only filter inside them as it does today.
- Whether a dependency replaced whole should still have its include directories
  on the test's compile line. Today they come with the dependency; the fake
  usually needs its header.
