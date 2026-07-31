# moltest

A small unit-testing framework for C, in the spirit of pytest.

```c
#include <moltest.h>

MOLTEST(str_list_push_appends) {
    str_list list;
    str_list_init(&list);
    EXPECT_TRUE(str_list_push(&list, "a"));
    EXPECT_EQ(1, str_list_count(&list));
    str_list_free(&list);
}
```

Tests register themselves, so there is no list to keep in sync. Link
`src/moltest.c` and `src/moltest_main.c` into your test binary with
`-Imodules/moltest/include`; `moltest_main.c` provides `main()`.

## Assertions

`EXPECT_*` records a failure and lets the test continue; `ASSERT_*` also stops
the test.

| Assertion | Checks |
|---|---|
| `EXPECT_TRUE` / `EXPECT_FALSE` | a condition |
| `EXPECT_EQ` / `EXPECT_NE` (alias `EXPECT_EQUALS`) | integers, or strings when the actual value is a `char *` |
| `EXPECT_STREQ` / `EXPECT_STRNE` | strings, explicitly |
| `EXPECT_PTR_EQ` / `EXPECT_PTR_NE` | pointer identity |
| `EXPECT_NULL` / `EXPECT_NOT_NULL` | pointers |
| `EXPECT_LT` / `LE` / `GT` / `GE` | ordering |

Outcome control: `SKIP("reason")`, `WARN("message")`, `FAIL("message")`, and
`MOLTEST_SKIP(name, "reason")` to skip a test without running it.

## Output

Results are grouped per file with a progress percentage; `.` passed, `F` failed,
`s` skipped, `W` passed with warnings. Failures print the expectation, the
expected and actual values, the location and anything the test printed (output
is captured and only shown when a test fails). Colour is used when writing to a
terminal, honouring `NO_COLOR`.

## Options

| Option | Meaning |
|---|---|
| `-k <substring>` | only run tests whose file or name matches |
| `-v` | one line per test |
| `-s` | do not capture what the tests print |
| `--list` | list the registered tests |
| `--color=auto\|always\|never` | colour output |

The exit status is 0 when nothing failed, 1 otherwise.

## Extending

`moltest_set_reporter()` installs an observer with `on_run_start`,
`on_file_start`, `on_test_end`, `on_file_end` and `on_run_end` callbacks, so
extra output (JUnit XML, coverage, …) can be added without touching the runner.
