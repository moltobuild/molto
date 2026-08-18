# Formatting and linting

A practical guide to `molto fmt` and `molto lint`, and to the two files that
configure them: `format.json` and `linter.json`.

This document describes **what Molto implements today**.
[RFC-0005](../rfcs/0005-code-style.md) is the design specification. When the two
disagree, this file describes the binary you are running.

## The one rule

**Molto owns the configuration; the tools do the work.**

Molto does not format or analyse code — it never will, and RFC-0001 says so.
What it owns is the *vocabulary*: you write `indent_width` and `brace_style`,
and Molto renders that as the backend's own configuration, into `.bin/`, right
before running it. Your repository never grows a `.clang-format`.

The practical consequence: switching backends later is a change to one line,
not a rewrite of a configuration you spent an afternoon on.

## Getting the tools

Molto does not install the formatter or the linter. It asks `pickup`:

```console
$ pickup tools
KIND       NAME          VERSION                      SOURCE
formatter  clang-format  clang-format version 22.1.8  pickup
linter     clang-tidy    LLVM version 22.1.8          pickup
```

Pickup unpacks `clang-format` and `clang-tidy` alongside the compiler, so a
machine with a pickup-installed LLVM already has both. If that table is empty:

```console
$ pickup install clang-format
```

Molto takes the path pickup reports and runs it. It does not search your `PATH`,
does not install anything and does not rewrite the path. To point it somewhere
else yourself, set `MOLTO_CLANG_FORMAT` or `MOLTO_CLANG_TIDY`; those bypass
resolution entirely and are not cached.

**No linter is not an error.** `molto lint` still runs the compiler's own
diagnostics, which is most of the value and needs nothing installed.

## `molto fmt`

```console
$ molto fmt              # rewrite src/ and include/ in place
$ molto fmt --check      # report what would change, write nothing; exits 1 if any
$ molto fmt --diff       # print the unified diff, write nothing
$ molto fmt -j 4         # at most four files at once; without it, every core
```

`--check` is the CI form: it fails the build when a file is not formatted,
without touching the working tree. `--diff` is the same question with the answer
shown as a patch.

Headers are formatted too. Style applies to a `.h` as much as to a `.c`, and a
header nobody includes still has to be readable.

Which is why the formatter is told what language it is reading. A `.h` carries
no language in its extension, and clang-format left to itself assumes the newest
C++: a C struct with a field named `requires` is then read as a C++20
requires-clause and the declaration is torn across three lines. Molto passes the
`[target].cpp_std` from the manifest, so a C project is formatted as C. A C++
project that never declares `cpp_std` is formatted as C too — the manifest is
the only thing that says otherwise, and this is one more reason to declare it.

## `molto lint`

```console
$ molto lint                     # compiler diagnostics, then the linter
$ molto lint --format json       # machine-readable, for CI
$ molto lint --profile release   # analyse with the release profile's defines
$ molto lint -j 4                # at most four files at once; without it, every core
```

### What it prints

Three shapes, and which one you get when you do not ask:

| `--format` | Shape | Default when |
|---|---|---|
| `rich` | the frame a build draws, with the source line and a caret | stdout is a terminal |
| `text` | one normalized line per finding: `path:line:col: severity: message [rule]` | stdout is anything else |
| `json` | one document, for CI | only when asked |

So `molto lint` at a terminal reads like a build, and `molto lint > report`,
`molto lint \| grep`, and every CI job that never passed the flag keep exactly
the output they have always had. `--format text` asks for that shape at a
terminal too. `json` is the contract a script should depend on; neither of the
other two moves it.

A `rich` block covers one file, names it at the top, and draws up to ten
findings in a frame before the rest fall back to the `text` line. The frame is
described in [Project.md](Project.md#what-a-build-says-when-something-is-wrong).

Which file a block is about is read off the findings themselves, and a compiler
says more than one thing about that. A diagnostic inside a header belongs to the
header, so that is the file the block names — and the `In file included from`
chain that reached it, written on lines naming no file at all, opens that block
rather than closing the one before it, because it is what says which source of
yours pulled the header in.

While the analysis runs, a terminal gets one row saying so — a braille spinner
and the word `analyzing` — drawn where the first diagnostic is about to go and
taken away before it arrives. It says nothing about how far along the work is,
because there is no honest figure to say: a run that replays most of its files
from the cache would be counting a denominator nobody waits for. A pipe, a file
and `--format json` get no row and no escape sequence, exactly as before.

Two passes over every source under `src/`:

1. **The compiler** declared in `[target]`, in a syntax-only pass. No object
   files, no `build/`, nothing written.
2. **The linter**, if there is one, configured from `linter.json` and handed the
   same compile flags the build would use.

`--profile` matters more than it looks: the profile decides which `defines` are
in force, and a `#ifdef` decides what even compiles. Linting with the wrong
profile analyses code the build never sees.

**Exit codes.** `0` when no `error`-severity diagnostic was produced, `1` when
one was, `2` for an invalid configuration, `4` for bad usage. A warning is
reported and still succeeds — only `error` fails the command.

### Neither command looks at the same file twice

A file that has not changed is not analysed or formatted again: what the tools
said about it is recorded in `.bin/` and replayed. What you see is identical
either way — same diagnostics, same order, same exit code — because replaying a
warning as silence would be a green build that hid it.

`molto lint` re-analyses a file when its content changes, when any header it
includes changes, when the command would differ (a different profile, defines,
flags), when `linter.json` changes, or when the linter's version does. Editing
one file in a large project costs one file's analysis, not the project's.

`molto fmt` records that a file is in its final form, and that record is not
about the mode: formatting a project leaves `molto fmt --check` with nothing to
run, which is the order CI usually does it in. It is invalidated by the file
changing, by `format.json` changing, or by a new version of the formatter.
Headers are not involved — a formatter reads the file it is given and none of
its includes.

```console
$ molto lint --refresh-analysis    # analyse everything again, ignoring what was recorded
$ molto fmt --refresh-analysis     # likewise
```

That is for a tool that is not deterministic, or one whose behaviour depends on
something Molto cannot see. Needing it routinely means the cache is wrong, and
the answer to that is a bug report rather than the flag.

## `format.json`

Optional. Absent means the defaults below.

```json
{
    "backend": "clang-format@22.1.8",
    "preset": "molto",
    "exclude": ["src/vendor/**"],
    "style": {
        "indent_width": 4,
        "line_width": 100,
        "brace_style": "attach",
        "pointer_alignment": "right",
        "sort_includes": true
    }
}
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `indent_width` | int | `4` | Columns per indentation level |
| `use_tabs` | bool | `false` | Indent with tabs instead of spaces |
| `line_width` | int | `100` | Maximum column before wrapping |
| `brace_style` | string | `attach` | `attach`, `break`, `linux`, `allman` |
| `pointer_alignment` | string | `right` | `left` (`int* p`), `right` (`int *p`) |
| `sort_includes` | bool | `true` | Sort `#include` blocks |
| `space_before_paren` | bool | `false` | Space between a keyword and `(` |
| `column_limit_comments` | bool | `true` | Wrap comments at `line_width` |

## `linter.json`

Optional. A severity map in the style of ESLint: each key names a rule or a
family, each value is `off`, `warn` or `error`.

```json
{
    "backend": "clang-tidy@22.1.8",
    "preset": "molto",
    "exclude": ["src/generated/**"],
    "rules": {
        "bugprone": "error",
        "readability_magic_numbers": "warn",
        "modernize": "off"
    }
}
```

The rule names are Molto's, not the backend's:

| Rule | What it covers |
|---|---|
| `bugprone` | Patterns that are usually a bug |
| `performance`, `portability`, `modernize`, `readability` | The families of the same name |
| `dataflow`, `security` | The path-sensitive analyzer, by half (slow; see below) |
| `naming_snake_case` | Identifier naming |
| `readability_magic_numbers`, `identifier_length` | Individual readability checks |
| `swappable_parameters`, `spurious_wakeup` | Two `bugprone` checks, named so they can be refused on their own |
| `unused`, `shadow`, `uninitialized`, `implicit_conversion`, `sign_compare` | Compiler diagnostics, by name |

A rule Molto cannot express for the selected backend is **an error naming the
rule and the backend**, never something quietly dropped.

Most rules name a family, because that is the unit a project usually has an
opinion about. A few name one check, for when a family is worth keeping and one
member of it is not:

```json
{
    "rules": {
        "bugprone": "warn",
        "swappable_parameters": "off",
        "spurious_wakeup": "off"
    }
}
```

Those two are the ones a C project most often wants gone. `swappable_parameters`
flags any two adjacent parameters of the same type, which is how most C
functions are written — including the signatures `qsort` and a callback table
impose, which cannot be changed at all. `spurious_wakeup` wants the wait inside
a `while`, and cannot see a retry loop that lives one function above it. Turning
either off leaves the rest of `bugprone` in place, which is the point: that
family does find real bugs.

## Shared keys

Both files accept these.

**`preset`** — `molto` (the default) or `none`. The `molto` preset asks the
compiler for `-Wall -Wextra -Wpedantic` and the linter for `clang-diagnostic-*`
and `bugprone-*`. `kernel` and `gnu` are named in RFC-0005 but not implemented;
declaring one is an error rather than a silent substitution.

The generated `Checks` list opens with `-*`. clang-tidy composes what it is
given on top of its own default rather than replacing it, so without that the
file would enable checks nobody configured — and a different clang-tidy, whose
default is its own to change, would enable a different set on the next machine.

The path-sensitive analyzer is deliberately not in the default: it is dozens of
times slower — 0.16 s against 8.3 s over three of this project's larger sources
— and pays for it with a handful of findings. Two rules ask for it:

`"dataflow": "warn"` walks paths rather than syntax, and finds what that buys:
a null dereference, a leak, a use of an uninitialized field, a dead store. It
covers the analyzer's `core`, `unix`, `valist` and `deadcode` families — not
`osx` or `cplusplus`, which describe another platform and another language, and
not `unix.Stream`, which reads the ordinary `while((n = fread(...)) > 0)` loop
as a read past the end of the file.

`"security": "warn"` adds its security family, minus the check that demands the
C11 Annex K functions (`snprintf_s` and friends) that glibc does not ship; that
one fires on every call to the C library and buries the rest.

Expect false positives from either: the analyzer does not inline variadic
functions, so a codebase that writes `ok = set_error(...)` — this one does —
gets told the failure branch continues.

**`exclude`** — glob patterns matched against the path relative to the project
root. A star crosses a slash, so `vendor/*` matches `vendor/a/b.c`; a trailing
`/**` also matches the directory itself.

**`backend`** — pins a name and, optionally, a version: `clang-tidy@22.1.8`.
Molto **verifies** the pin against what pickup reports; it does not install.
An unmet pin is an error naming both versions, because two releases of one
formatter produce different output for the same file, and that is precisely the
noise a formatter exists to remove.

## Style configuration fails closed

An unknown key, an unknown value, a rule with no translation, a list that
overflows, a pin that is not met — each is an error with a message naming what
is wrong. Nothing in `format.json` or `linter.json` is ignored.

That is deliberate. A key that silently does nothing is worse than one that is
refused: you would keep the line in your configuration for years believing it
applied.

`Project.toml` does not behave this way — it drops unknown keys without warning,
for the reason given in [`docs/Project.md`](Project.md#not-implemented-yet).
These two files describe a style that is either expressible or not; the manifest
describes a schema that is still growing.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `this machine has no formatter` | Pickup reports none | `pickup install clang-format`, or set `MOLTO_CLANG_FORMAT` |
| `unknown key 'styl'` | A typo in the configuration | Check it against the tables above |
| `rule 'x' is not supported by clang-tidy` | A rule name that is not canonical | Use one from the rule table |
| `backend is pinned to '…' but this machine has '…'` | The installed version is not the pinned one | Install the pinned version, or change the pin |
| `preset is not implemented yet` | `kernel` or `gnu` | Use `molto` or `none` |
| Lint reports nothing from the linter | No linter installed | Check `pickup tools`; the compiler pass still ran |
| Lint reports what the build does not | Wrong profile | `molto lint --profile release` |
| `--check` and `--diff` together | Two answers to one question | Pick one |
| Lint reports something you already fixed | A cached result that should have been invalidated | `molto lint --refresh-analysis`, and report it: the cache is meant to make that impossible |
| Lint is not faster on the second run | Nothing could be recorded — most often a compiler that does not write `-MF` dependency lists | Check that `.bin/lint/` contains `.d` files |
| Lint or fmt takes the whole machine | That is the default | `-j 4`, on either command |
| An external clang-tidy disagrees with `molto lint` | It is reading `compile_commands.json`, which a *build* writes — not this command | Run `molto build` (or `molto test`) so the database matches what you are linting |

## Related

- [RFC-0005: Code Style](../rfcs/0005-code-style.md) — the design specification
- [`docs/Project.md`](Project.md) — configuring `Project.toml`
- [RFC-0004: Workspace](../rfcs/0004-workspace-specification.md) — what lives in `.bin/`
