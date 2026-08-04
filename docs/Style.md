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
```

`--check` is the CI form: it fails the build when a file is not formatted,
without touching the working tree. `--diff` is the same question with the answer
shown as a patch.

Headers are formatted too. Style applies to a `.h` as much as to a `.c`, and a
header nobody includes still has to be readable.

## `molto lint`

```console
$ molto lint                     # compiler diagnostics, then the linter
$ molto lint --format json       # machine-readable, for CI
$ molto lint --profile release   # analyse with the release profile's defines
```

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
| `security` | The path-sensitive security analyzer (slow; see below) |
| `naming_snake_case` | Identifier naming |
| `readability_magic_numbers`, `identifier_length` | Individual readability checks |
| `unused`, `shadow`, `uninitialized`, `implicit_conversion`, `sign_compare` | Compiler diagnostics, by name |

A rule Molto cannot express for the selected backend is **an error naming the
rule and the backend**, never something quietly dropped.

## Shared keys

Both files accept these.

**`preset`** — `molto` (the default) or `none`. The `molto` preset asks the
compiler for `-Wall -Wextra -Wpedantic` and the linter for `clang-diagnostic-*`
and `bugprone-*`. `kernel` and `gnu` are named in RFC-0005 but not implemented;
declaring one is an error rather than a silent substitution.

The path-sensitive analyzer is deliberately not in the default: its security
checks demand the C11 Annex K functions (`snprintf_s` and friends) that glibc
does not ship, so it would bury real findings under hundreds of unfixable
warnings. Ask for it explicitly with `"security": "warn"`.

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

## Related

- [RFC-0005: Code Style](../rfcs/0005-code-style.md) — the design specification
- [`docs/Project.md`](Project.md) — configuring `Project.toml`
- [RFC-0004: Workspace](../rfcs/0004-workspace-specification.md) — what lives in `.bin/`
