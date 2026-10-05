# RFC 0020: A Test Executable's Place in the Run

- RFC Number: 0020
- Title: A Test Executable's Place in the Run
- Status: Accepted
- Created: 2026-10-05

## Summary

`molto test` tells each test executable it runs where it stands in the run,
through two environment variables:

```
MOLTO_TEST_INDEX=1  MOLTO_TEST_COUNT=2     the first of two
MOLTO_TEST_INDEX=2  MOLTO_TEST_COUNT=2     the last
```

A plugin that measures the whole run, rather than one executable, needs to know
which executable starts it and which one ends it. Only Molto knows. Nothing
else changes: Molto still does not read what the executables measure.

## Motivation

A suite can be several executables: `mode = "per_file"` builds one per test
file (RFC-0003), and RFC-0021 would give every isolated test its own. A plugin
running inside each of them sees one executable and nothing of the others.

moltest-coverage (RFC-0019) shows what that costs. At the start of a run it
erases the counters of the last one; at the end it reads them, prints the
report and applies `fail_under`. In one executable that is right. In two, run
by `molto test --profile coverage` on a `per_file` project whose two test files
each cover one of two functions:

```
tests/test_a.c   src/two.c  3/6  50.0%   Missing 6-8
run failed: moltest_coverage: line coverage 50.0% is under fail_under = 90.0
tests/test_b.c   src/two.c  3/6  50.0%   Missing 2-4
run failed: moltest_coverage: line coverage 50.0% is under fail_under = 90.0
0 passed, 2 failed
```

The suite covers six lines of six. The first executable judges half a
measurement; the second erases the first's counters before taking its own, so
lines 2-4, which `test_a` ran, are reported missing. Both fail. The counters
themselves are fine: libgcov merges into one `.gcda` across processes that run
the same object. What is wrong is who erases and who judges, and the plugin
cannot tell from inside an executable whether it is the first or the last.

## Design

### The variables

For each executable it runs, `molto test` sets, in that executable's
environment only:

| Variable           | Value                                                 |
|--------------------|-------------------------------------------------------|
| `MOLTO_TEST_INDEX` | its position in this run, from 1                      |
| `MOLTO_TEST_COUNT` | how many executables this run starts, the same for all |

They are set the way `[env]` is (RFC-0003): in the child, after it is created,
never in Molto's own environment. They win over an `[env]` entry of the same
name, which would otherwise make the position a lie. They describe a run, not a
build, so they are not part of any fingerprint (RFC-0007): changing the number
of test files re-runs nothing that is up to date.

The order is the order `molto test` prints the executables in. They run one
after another, and one that fails does not stop the rest, so the executable
whose index equals the count is always the last to run.

### The order Molto keeps

The position is only worth something if the first executable really starts
the run and the last really ends it. Molto guarantees both, whatever way it
schedules executables now or later:

- the executable with index 1 runs alone, and finishes before any other starts;
- the executable with index `MOLTO_TEST_COUNT` starts only after every other
  has finished;
- those in between may run one after another, as today, or at once.

Today every executable runs alone, so the guarantee costs nothing. If
`molto test` starts executables in parallel one day, it keeps the first and
the last apart and parallelises the rest: two executables run on their own,
and no plugin written against this RFC has to change.

A run with a single executable, the `single` mode, says `1` and `1`. An
executable started by hand, outside `molto test`, sees neither variable.

### What a plugin does with them

The contract a plugin follows is short:

- the variables absent: a run of its own, first and last at once, which is what
  every plugin assumes today;
- `MOLTO_TEST_INDEX` equal to 1: the first, the one to reset what a previous run
  left behind;
- `MOLTO_TEST_INDEX` equal to `MOLTO_TEST_COUNT`: the last, the one with the
  whole run behind it to report on or judge;
- anything else, or values that do not parse: neither. A plugin that cannot
  tell should behave as if the variables were absent and say so, rather than
  report on a partial run as if it were whole.

For moltest-coverage that is: erase the counters only in the first, report,
write the lcov and apply the floors only in the last, and print one line in
the others saying the report comes with the last executable.

### What Molto does not do

- **Report, merge or erase anything.** It names a position. The counters, and
  what they mean, stay the test framework's (RFC-0019).
- **Order executables for a plugin's sake.** The order is the one Molto already
  runs them in; it only keeps the first and the last on their own.
- **Set anything for `molto run`.** It starts one program, which has no place
  in a run of several.

## Molto testing itself

Molto's suite is `single`, so its own coverage run sees `1` and `1` and behaves
as it does today. The change is verified where it shows: a `per_file` project
with moltest-coverage whose test files cover different functions passes its
floor with the coverage of the whole suite, and its lcov lists every line any
executable ran.

## Alternatives considered

**Fix it in moltest-coverage alone.** It could guess the first and the last
executable from file timestamps or lock files under `build/`. Every such guess
fails somewhere: a binary run by hand, a run interrupted halfway, two runs at
once.

**Molto reports at the end.** `molto test` could run gcov after the last
executable. RFC-0019 decided against it: the report, its format and its floor
belong to the test framework.

**Molto erases the counters before the run.** That fixes the erasing, not the
judging: the first executable would still apply the floor to part of the run.
Knowing the last one is needed anyway, and with it the first comes for free.

**A run directory, or a count of finished executables.** Molto could hand each
executable a directory shared by the run, where plugins leave state and count
themselves, and the last to finish reports. It allows every executable to run
at once, at the cost of a locking protocol every plugin has to get right, on
every platform. Keeping the first and the last on their own gives the same
result to plugins that only read two variables.

**Extra runs before and after.** Molto could start an executable once more,
before and after the suite, only to erase and to report. An executable that
does not know the convention, such as a `per_file` test with its own `main()`,
would run its tests again. The position is passive: an executable that ignores
it loses nothing.

## Implementation Status

Implemented after molto 0.49.0: `molto test` sets both variables for each test
executable it runs, in its environment only, after dropping any `[env]` entry
of either name (`test_command`). Executables run one at a time in the order
printed, which keeps the first and the last on their own.
`test_command_tells_each_binary_its_place_in_the_run` runs two executables with
`[env]` setting both to 99 and reads back `1/2` and `2/2`.

moltest-coverage's side (erase in the first, report in the last) is its own
change: its ADR 0004 and KI-3.
