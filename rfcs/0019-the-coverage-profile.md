# RFC 0019: The Coverage Profile

- RFC Number: 0019
- Title: The Coverage Profile
- Status: Draft
- Created: 2026-10-04

## Summary

This RFC adds a fifth built-in build profile, `coverage`, whose builds are
instrumented for code coverage. `molto test --profile coverage` is then the
whole of what a project does to measure itself; reading the measurement is a
test framework's job, and moltest-coverage is the one the ecosystem ships.

Molto builds instrumented code. It does not report on it.

## Motivation

Coverage in C and C++ has three steps. The compiler instruments the code
(`--coverage`), the instrumented binary writes counters when it runs (`.gcda`),
and a tool turns the counters into lines, branches and functions (`gcov`).
Only the first is a build system's, and today Molto does not take it.

What a project writes instead is a profile of its own:

```toml
[profile.custom]
opt_level = 0
debug_info = true
flags = ["--coverage"]
```

That works — moltest-coverage's own CI runs on it — and it is wrong in three
ways. It spends `custom`, the one profile a project has for its own purposes, on
something every project wants the same way. It is a recipe every project has to
copy correctly: `-O0` matters, because an optimiser that merges or removes lines
makes a covered line look missed. And it names a GCC/Clang flag in a manifest
that RFC-0003 tries to keep compiler-neutral, which is the kind of decision a
build system is for.

Molto itself measures its coverage outside Molto: `make coverage` compiles a
second, instrumented copy of the tree with the bootstrap Makefile, and
`.github/coverage.sh` reads it. The Makefile is meant to be the bootstrap and
nothing more (README, `Project.toml`), and coverage is the largest thing it does
that `molto` cannot.

## Design

### The profile

`coverage` joins `debug`, `release`, `bench` and `custom`:

| Profile | `opt_level` | `debug_info` | Instrumented |
|---|---|---|---|
| `coverage` | 0 | true | yes |

Every command that takes `--profile` takes `coverage`: `build`, `run`, `test`,
`lint`, `ir`. Output goes to `build/coverage/`, beside the others, so an
instrumented object never overwrites an uninstrumented one — the property the
Makefile's separate `build/coverage/obj` tree exists for.

### Instrumentation is the profile's, not the manifest's

The instrumentation flag is not written in the manifest and not added to
`[profile.coverage].flags`. Molto adds it, to the compile and the link lines of
every target the project builds in that profile, as a `profile`-scope option
(RFC-0013): the place a user's own `[profile.coverage].flags` also land, and
after them, so a user cannot accidentally turn it off with a later flag and can
see it in `molto ir --profile coverage`.

The flag is the toolchain's to spell:

| Compiler family | Instrumentation |
|---|---|
| GCC, Clang (incl. Apple, MinGW) | `--coverage` (compile and link) |
| MSVC | refused, before anything is built |

MSVC has no gcov-format coverage, and a profile that silently built an
uninstrumented binary would report "no data" for a reason nobody could find:

```
molto: the coverage profile needs a GCC- or Clang-compatible compiler;
       the resolved toolchain is msvc
```

`[profile.coverage]` remains an ordinary profile table: `opt_level`,
`debug_info`, `defines`, `include` and `flags` are read and added on top as for
any other profile. A project that wants `-O1` coverage builds says so there.

### What is instrumented

The project's own targets: `src/` and `tests/`. Not its dependencies, which are
compiled against their own recipes and not against the consumer's profile
(the rule `ir_transform` already states), so a library's coverage figure is its
own code and nothing it links. A dependency that needs the coverage runtime to
be linked — moltest-coverage, which calls `__gcov_dump` — exports `--coverage`
in its recipe's `[artifacts] flags`, which since #87 reaches the link line.

### What Molto does not do

Molto does not run gcov, does not print a figure and does not gate on one.
Reading counters belongs to whatever runs the tests, because only it knows when
a run ends, which tests ran, and what the project wants to fail on. A project
using moltest adds moltest-coverage; one using another framework runs gcovr or
lcov over `build/coverage/` after `molto test --profile coverage`. The profile
is the same for all of them.

## Molto measuring itself

Once this exists, Molto's own coverage moves off the Makefile:

1. `Project.toml` gains moltest-coverage in `[dev-deps]`, and
   `moltest-coverage.toml` carries the floor: `fail_under = 79.5`, the figure
   `coverage.floor` holds today, with the same rule — it only ever goes up, in a
   commit that says why.
2. The `coverage` CI job runs `./build/molto test --profile coverage` with the
   molto the job just bootstrapped, and uploads `coverage.lcov`.
3. `make coverage`, `.github/coverage.sh` and `coverage.floor` are removed.

Step 1 needs the moltest in `modules/moltest` to be one moltest-coverage can
plug into (reporter API v1, moltest 0.3.0). That copy is older, so this step
depends on Molto consuming moltest as a dependency rather than as a vendored
copy, which is its own change (the bootstrap Makefile still compiles the suite
from `modules/moltest`) and is not specified here.

## Alternatives considered

**Leave it to `[profile.custom]`.** It works and costs Molto nothing, and every
project pays instead: the recipe copied by hand, the one free profile spent.

**`molto test --coverage`.** A flag reads well, but it is a profile by another
name — instrumented objects still need their own directory — and a second way
to select one would let `--coverage --profile release` mean something nobody
can predict.

**A Molto-native report.** `molto test --profile coverage` could run gcov and
print a table. It would duplicate what a test framework plugin does better
(per-test contexts, failing the run, its own config), and tie the build system
to one report format and one gate. The profile is the part only a build system
can do.

**Instrument dependencies too.** Whole-program coverage is occasionally wanted,
and would break the property that a package compiles identically in every
project that depends on it, which is what lets an object be shared between
them. A project that wants it can add `--coverage` to `[profile.coverage]`
flags of its own path dependencies' recipes.

## Unresolved questions

- Whether `molto clean` should offer to remove only `.gcda` files: counters
  accumulate across runs unless something erases them (moltest-coverage does,
  at the start of each run).
- Whether `--coverage` should be the only spelling, or whether Clang's
  source-based coverage (`-fprofile-instr-generate -fcoverage-mapping`) deserves
  a second profile or a key once a reader for it exists.
