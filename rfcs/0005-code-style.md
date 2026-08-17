# RFC 0005: Code Style

- RFC Number: 0005
- Title: Code Style
- Status: Draft
- Created: 2026-07-31

## Summary

This RFC specifies how Molto handles **code style**: the `molto fmt` and
`molto lint` commands, the two configuration files that drive them
(`format.json` and `linter.json`), the **canonical style model** they express,
and the **translation** of that model into the native configuration of a
user-selected backend. It expands the one-paragraph definition of `molto lint`
in RFC-0002, introduces `molto fmt`, and gives `spec.md` section 17 (Coding
Conventions) an executable meaning.

## Motivation

The C and C++ ecosystem does not lack analysis engines. It has excellent ones —
`clang-format`, `clang-tidy`, `cppcheck`, `uncrustify`, Coccinelle. What it
lacks is a common way to configure them.

Every engine invents its own format, option names and defaults.
`.clang-format` and `uncrustify.cfg` describe overlapping ideas in incompatible
vocabularies, so switching engines means rewriting the configuration from
scratch and rediscovering by trial and error which knob maps to which. That
lock-in is expensive enough that most projects never revisit a choice made once,
years earlier.

Molto does not need to write another engine to fix this. It needs to own the
**configuration layer**: one vocabulary, one file per concern, translated to
whichever backend the project chooses.

## Design: Molto as a Style Frontend

Molto **MUST NOT** implement its own lexer, parser, formatter or static
analyzer. It is a *frontend* for style configuration and an *orchestrator* of
existing engines.

This mirrors what Molto already does with compilers. `Project.toml` declares
intent — an optimization level, debug info, a language standard — and Molto
translates that intent into the flags of `gcc`, `clang` or `cl` (RFC-0003,
`[profile.*]`). Molto abstracts compilers without writing one. Code style
follows the same pattern: the project declares intent, Molto translates it for
the selected engine.

Keeping this symmetry matters. It introduces no exception to the architecture,
and it stays inside the boundary drawn by RFC-0001: *"Molto orchestrates
existing toolchains; it does not replace them."* Parsing C is hard and parsing
C++ is, in the general case, undecidable; a project whose stated non-goal is
being a compiler has no business attempting either.

What Molto contributes instead is the part nobody owns today: a portable,
version-pinned, zero-setup configuration layer.

## Configuration Files

Two files at the workspace root, discovered alongside `Project.toml`
(RFC-0004, Discovery):

- `format.json` — how code is laid out. Consumed by `molto fmt`.
- `linter.json` — which checks run and how severely. Consumed by `molto lint`.

Both are **optional**; when absent, Molto applies its default preset. They are
hand-edited and never silently rewritten, the same rule RFC-0003 applies to the
manifest. Keeping them out of `Project.toml` is deliberate: the manifest is the
source of truth about the *project* (identity, dependencies, targets), while
these describe the *style*, which teams share across projects and version
independently.

`format.json`:

```json
{
    "backend": "clang-format@18.1.8",
    "preset": "molto",
    "exclude": ["vendor/**", "third_party/**"],
    "style": {
        "indent_width": 4,
        "line_width": 100,
        "brace_style": "attach",
        "pointer_alignment": "right",
        "sort_includes": true
    }
}
```

`linter.json`:

```json
{
    "backend": "clang-tidy@18.1.8",
    "preset": "molto",
    "rules": {
        "bugprone": "error",
        "naming_snake_case": "error",
        "readability_magic_numbers": "warn",
        "modernize": "off"
    }
}
```

## The Canonical Style Model

The option names above belong to **Molto**, not to any backend. This is the
central idea of this RFC and it needs stating precisely: the canonical model is
neither the union nor the intersection of the backends. It is an independent
vocabulary, designed around what developers actually configure, which each
backend then implements to a greater or lesser degree.

The union would be unusable — a model carrying every option of every engine
would be larger than any of them. The intersection would be useless, shrinking
toward the smallest engine. So the model is curated: Molto specifies the options
that matter and reports honestly where a backend cannot honor them.

The initial formatting vocabulary:

| Key                 | Type   | Default    | Description                              |
|---------------------|--------|------------|------------------------------------------|
| `indent_width`      | int    | `4`        | Columns per indentation level            |
| `use_tabs`          | bool   | `false`    | Indent with tabs instead of spaces       |
| `line_width`        | int    | `100`      | Maximum column before wrapping           |
| `brace_style`       | string | `attach`   | `attach`, `break`, `linux`, `allman`     |
| `pointer_alignment` | string | `right`    | `left` (`int* p`), `right` (`int *p`)    |
| `sort_includes`     | bool   | `true`     | Sort `#include` blocks                   |
| `space_before_paren`| bool   | `false`    | Space between a keyword and `(`          |
| `column_limit_comments` | bool | `true`   | Wrap comments at `line_width`            |

Lint uses a severity map instead, in the style of ESLint. Each key names a rule
or a rule family, each value is `off`, `warn` or `error`. Only `error` fails the
command; `warn` reports and returns success.

Both files accept a `preset` naming a curated starting point (`molto`, `kernel`,
`gnu`, `none`), with the explicit keys layered on top. `molto` is the default
and encodes `spec.md` section 17.

## Backend Translation

For each invocation Molto resolves the canonical model into the backend's
native configuration, writes it to a temporary file under `.bin/` (RFC-0004),
and runs the backend against it. The generated file is machine-owned and never
surfaces in the project tree, so `.clang-format` files do not accumulate in
repositories that use Molto.

Coverage is not uniform, and Molto **MUST NOT** pretend otherwise. When a
configured option has no faithful translation for the selected backend, Molto
**MUST** fail with a diagnostic naming both the option and the backend, rather
than dropping it silently:

```
molto: format.json: 'brace_style = "linux"' is not supported by
       uncrustify@0.78.1 (no equivalent option); remove it or switch backend
```

Silently ignoring an option would leave the user with a style they did not ask
for and no indication why. Failing closed is the same choice Molto's TOML parser
already makes for malformed manifests.

Translation is also **bidirectional**. `molto fmt --import <file>` reads an
existing `.clang-format` or `uncrustify.cfg` and emits the equivalent
`format.json`, reporting options it could not represent. This is the adoption
path for projects that already have a style, and it complements the
`molto migrate` command specified in RFC-0002.

## Supported Backends

Backends are adopted in tiers, so breadth never comes at the cost of a
half-working translation layer.

| Backend        | Role   | Tier | Status                                  |
|----------------|--------|------|-----------------------------------------|
| `clang-format` | format | 1    | Initial target; widest option coverage  |
| `clang-tidy`   | lint   | 1    | Initial target                          |
| `uncrustify`   | format | 2    | Reserved                                |
| `cppcheck`     | lint   | 2    | Reserved                                |
| `coccinelle`   | lint   | 3    | Reserved; pattern-based, C only         |

Tier 1 is what this RFC commits to. Tiers 2 and 3 are acknowledged directions
whose translation tables will be specified in follow-up RFCs, once the canonical
model has proven stable against a second backend.

Independently of any backend, `molto lint` always surfaces the diagnostics of
the compiler declared in `[target]`, obtained with a syntax-only pass. Those
cost nothing, require no installation, and are what RFC-0002 already promised.

## Backend Acquisition and Pinning

A backend **MUST** be pinned to an exact version (`clang-format@18.1.8`). Molto
resolves it, installs it under the global cache `~/.molto/` (`spec.md`
section 10) if it is not already present, and records the effective version in
the WSDB (RFC-0004).

Pinning is not optional polish; it is what makes formatting reproducible.
Different releases of the same formatter produce different output for the same
input, so an unpinned backend means two developers on one team generate
different diffs from the same file — precisely the noise a formatter exists to
eliminate. Because Molto provisions the backend instead of assuming a system
installation, this holds even where the tool is not normally available: a
Windows host using MinGW, or a macOS host whose Xcode toolchain ships no
formatter.

The download, verification and installation mechanism is **not specified here**.
It is a contract with a general toolchain manager that applies equally to
compilers, and belongs in its own RFC. This RFC requires only that resolution be
exact, cached and recorded.

## `molto fmt`

Formats the project's sources in place.

- `--check` — do not write; exit non-zero if any file would change. For CI.
- `--diff` — print the unified diff instead of writing.
- Operates on sources discovered from the filesystem per RFC-0001, minus
  `exclude`. Headers are included: style applies to `.h`/`.hpp` as much as to
  `.c`/`.cpp`.

`molto fmt` is added to the command set defined in RFC-0002.

## `molto lint`

Runs compiler diagnostics and the configured lint backend, without producing
build artifacts, as specified in RFC-0002.

- `--fix` — apply automatic fixes, delegated to the backend when it supports
  them.
- `--format json` — emit machine-readable diagnostics for CI. This is the
  contract a script depends on, and no other shape moves it.
- `--format rich` — the frame a build draws a diagnostic in, with the source
  line and a caret (RFC-0011). It is what a terminal gets when `--format` is not
  given; anything redirected gets the normalized one-line form below, so a
  pipeline is never handed a shape it did not ask for.

## Current State

Both commands are implemented. What is in place, and what is not:

**Implemented.** `molto fmt` with `--check` and `--diff`; `molto lint` with the
compiler's syntax-only pass, the `clang-tidy` backend and `--format json`; the
canonical model of both configuration files, translated to the backends' own
configuration under `.bin/` and failing closed on anything it cannot express;
per-file work dispatched across the task pool; and the per-file cache described
under Caching, for both commands, on the store specified in
[RFC-0006](0006-analysis-result-cache.md) — which replays the diagnostics rather
than the fact that a file was once clean, so a cached run is indistinguishable
from an uncached one.

**Backend acquisition is pickup's, not Molto's.** The section above leaves the
download and installation mechanism unspecified, as a contract with a general
toolchain manager. That manager is `pickup`: `pickup tools` reports the
formatter and the linter this machine has and where they are, and pickup unpacks
`clang-format` and `clang-tidy` alongside the compiler. Molto asks, records the
answer in the WSDB, and runs the path it was given — it does not search,
install or rewrite it. A pinned `backend` is therefore *verified* against what
pickup reports rather than fetched; an unmet pin is an error naming both
versions.

**Not implemented yet**, and deliberately so:

- **`molto fmt --import`**, `molto lint --fix`, the `kernel` and `gnu` presets,
  and tier 2 and 3 backends. A configuration naming any of them is refused by
  name rather than quietly approximated.
- **Linting `tests/`**. Only `src/` is analysed; `molto fmt` covers `src/` and
  `include/`.

The `molto` preset asks the compiler for `-Wall -Wextra -Wpedantic` and the
linter for `clang-diagnostic-*` and `bugprone-*`. The path-sensitive analyzer is
not in the default: its security checks demand the C11 Annex K functions glibc
does not ship, so including it would bury real findings under hundreds of "use
`snprintf_s`". A project that wants it asks for it with the `security` rule.

## Diagnostics Model

Diagnostics from every source — compiler, formatter, lint backend — are
normalized to one format, so output does not change shape when a backend is
swapped:

```
src/net/socket.c:42:9: error: 'maxRetries' is not snake_case [naming_snake_case]
```

Exit codes follow RFC-0002: `0` when no `error`-severity diagnostic was
produced, `1` on violations or backend failure, `2` on an invalid or missing
configuration file, `4` on invalid CLI usage.

## Caching

Lint and format results are cached in the WSDB (RFC-0004), keyed per file and
validated by the same freshness signature the build uses: modification time and
size, confirmed by a content hash when those differ. The translated backend
configuration serves as the command fingerprint, so editing `format.json`,
changing `preset`, or bumping the pinned backend version invalidates the cache
automatically — exactly as changing a compiler flag invalidates an object file.

Unchanged files are not re-analyzed. Per-file work is dispatched across the
existing task pool.

## Non-Goals

- Molto does **not** implement lexers, parsers, formatters or static analyzers.
- Molto does **not** guarantee feature parity between backends. The canonical
  model is a shared vocabulary, not a compatibility layer that emulates missing
  capabilities.
- Molto does **not** target projects with mature in-house style infrastructure
  (Chromium, V8, LLVM). Those already have pinned tooling and their own CI; the
  audience here is new and mid-sized projects, for which usable style tooling
  currently costs a day of setup.
- Molto does **not** read a backend's native configuration file at runtime.
  Configuration flows in one direction, from the canonical model outward.

## Reserved / Future

- **Native rules** for conventions no backend implements — notably the include
  style of `spec.md` section 17 (`<controllers/user_controller>` rather than
  `"../controllers/UserController.h"`), which is lexical and needs no parser.
- Tier 2 and 3 backends and their translation tables.
- Shareable presets distributed through the registry (`spec.md` sections 15-16),
  so an organization can publish one style and depend on it by name.
- Per-directory configuration overrides.
- A formatter and linter for `Project.toml` itself.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md)
- [RFC-0003: Project Manifest](0003-project-manifest.md)
- [RFC-0004: Workspace Specification](0004-workspace-specification.md)
- [RFC-0006: Analysis Result Cache](0006-analysis-result-cache.md) — the store the caching above needs
- [RFC-0007: Build System](0007-build-system.md) — the discovery walk this widens to headers
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — how a formatter or a linter is published and obtained
- [RFC-0011: Build Diagnostics](0011-build-diagnostics.md) — the `rich` shape, which sits beside the normalized line rather than replacing it

See also `spec.md` sections 10 (Global Cache), 11 (Workspace Database) and 17
(Coding Conventions).
