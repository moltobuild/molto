# RFC 0014: Plugin System

- RFC Number: 0014
- Title: Plugin System
- Status: Draft
- Created: 2026-08-20

## Summary

This RFC specifies what a **plugin** is: the four things it declares — metadata,
**capabilities**, **extensions** and **permissions** — how it is **executed**,
what it is **allowed** to reach, how it is **distributed** and installed, how it
reports **diagnostics**, and how a project names one.

It formalises `spec.md` §14, which describes plugins in five lines and promises
"a stable API". This document says what that API is, and the answer is a
document rather than an ABI: a plugin is a process that reads an IR (RFC-0013)
and writes one. It also answers the sentence RFC-0007 left dangling — "the
answer is a new manifest key or a plugin — not a scripting layer" — which named
plugins as the escape hatch for everything the build refuses to do, without
anyone specifying what the escape hatch was.

## Motivation

Molto's stated goal is to build C and C++ projects, and the world's C and C++
projects are not written in Molto. They are written in Meson, in CMake, in
Autotools and in thirty years of Makefiles, and every one of them is a project
Molto cannot touch. Meanwhile the ecosystem wants things that are not builds at
all — a `.deb`, an AppImage, a Lambda bundle, a CUDA target, a WASM target — and
each of them, added to Molto directly, is a feature the core has to carry
forever for the fraction of users who need it.

Both problems have the same shape: something outside Molto knows how to describe
or produce something Molto does not. What is missing is not a feature, it is a
place to put one.

There is a sharper reason to specify this now rather than later, and it inverts
an argument already written down. RFC-0009 lets a recipe name a build system —
`make`, `cmake`, `autotools`, `meson` — and argues that naming an existing build
system avoids arbitrary code execution because "the escape hatch is `args`,
which is bounded". That is not true. `system = "cmake"` runs CMake, and a
`CMakeLists.txt` can call `execute_process`; `system = "meson"` runs Meson, and
a `meson.build` can call `run_command`. Delegating to an installed build system
*is* remote code execution with extra steps — the exact thing the RFC set out to
prevent — and it is the only option a recipe has today.

A frontend that reads `meson.build` and interprets a subset of the language,
with no `run_command` and no system functions at all, is **more** faithful to
RFC-0009's stated goal than delegation is. That is the case for compatibility
plugins, and it is worth making explicitly, because it reverses which of the two
looks like the safe default.

## What a plugin is

A plugin is an executable and a recipe. The recipe declares four things:

```toml
schema = 1
form = "binary"
kind = "tool"
name = "meson"
version = "0.3.0"
target = "x86_64-unknown-linux-gnu"

[tool]
kind = "plugin"

[plugin]
capabilities = ["frontend"]
extensions = ["meson.build"]
permissions = ["ir.write", "filesystem.project"]
ir_schema = 1
molto_min = "0.17.0"
```

**Metadata** is the coordinate every artifact in the ecosystem carries
(RFC-0009): kind, name, version, target. A plugin is a native executable, so its
`target` is a real triple and **MUST NOT** be `any` — an interpreted plugin
would need an interpreter, which is a dependency the recipe of a `tool` has no
way to express.

**Capabilities** are what the plugin can be asked to do. **Extensions** are the
filenames that select it as a frontend. **Permissions** are what it may reach
while it does the work, and the list is exhaustive: a permission that is not
declared is not granted, and there is no way to ask for one at runtime.

`ir_schema` and `molto_min` are what make the version mismatch of RFC-0013 a
refusal before the process starts rather than a half-read document.

## The eight capabilities

There is no such thing as "the Meson plugin". There are plugins that provide
capabilities, and a plugin may provide several.

| Capability | What it is asked | What it returns |
|---|---|---|
| `frontend` | a project directory | an IR document |
| `transform` | an IR document | an IR document |
| `target` | an IR document and a target triple | an IR document specialised for it |
| `generator` | a `BuildStep` to run | files, and their diagnostics |
| `compiler` | a translation unit | an object, and its diagnostics |
| `linker` | a set of objects | a binary, and its diagnostics |
| `packager` | a built artifact | a file in some distribution format |
| `command` | a `molto <name>` invocation | an exit code |

Splitting it this way rather than by tool is what keeps the model honest.
`cuda` is a `target` and a `compiler` and nothing else; `closures` is a
`transform` and a `generator`; `meson` is a `frontend`; `deb` is a `packager`.
None of them needs a concept of itself inside Molto, and adding a ninth kind of
thing to the ecosystem does not mean editing an enumeration of tool names.

Four of these — `generator`, `compiler`, `linker`, `command` — run a program.
That is the fact the whole security section of this RFC exists to handle, and it
is why a capability is declared in a recipe that is readable before the bytes
are downloaded.

## Two families

The capabilities sort into two families, and the distinction is worth a name
because the two carry different risks and deserve different scrutiny.

A **compatibility plugin** teaches Molto to understand something that already
exists: Meson, CMake, Autotools, a Makefile. It is almost always a `frontend`,
it reads files the user already has, and its whole job is translation. Its
failure mode is a wrong translation — a build that does the wrong thing — and
its blast radius is one project.

A **native plugin** adds a capability the ecosystem did not have: closures,
CUDA, WASM, Android. It is a `transform`, a `target`, a `generator` or a
`packager`, and it generally produces things rather than reading them. Its
failure mode is a bad artifact, and it usually runs programs to get there.

The families are not a permission boundary — a compatibility plugin can be as
dangerous as any other — but they predict what a plugin will ask for, and a
`frontend` requesting `process.spawn` is a question worth asking out loud.

## A frontend does not run the tool it understands

A Meson frontend does not invoke `meson`. It reads `meson.build` and interprets
it.

This is the single most consequential decision in this RFC, and it costs real
work, so the reasoning should be legible. Invoking the real tool is easier, more
complete and correct by construction — and it hands control of the build to a
file in the dependency, which can run anything, on the machine of anyone who
builds it. The Meson subset a frontend implements is chosen so that it cannot:

`project`, `executable`, `library`, `shared_library`, `static_library`, `files`,
`dependency`, `include_directories`, `declare_dependency`, `custom_target`,
`configure_file`, `if`, `foreach`, variables and user-defined functions.

Note what is absent, and that the absences are the point. There is no
`run_command`, no `find_program` reaching into an arbitrary `PATH`, no
`import('python')`, no module system, no `get_option` reading an environment
Molto does not control. A construct the frontend does not implement is a
**rejected build file with the line reported**, never a fallback to the real
tool — the same rule RFC-0009 applies to an unknown build system, for the same
reason: falling back means the safe path silently becomes the unsafe one at the
moment a project gets complicated enough to need it.

`custom_target` survives the cut because it becomes a `BuildStep` (RFC-0013),
with declared inputs and outputs, validated before it runs, and refused unless
the plugin declared `generator`. It is a command, it is visible as one, and it
is the only kind of command that gets through.

The consequence is that some projects will not build. A `meson.build` that
shells out to a script is outside this subset, and it will stay outside it. That
is a real limitation and it is the correct one: the alternative is that Molto
builds everything, including the thing that erases a home directory.

## Execution

A plugin is a process. Molto spawns it, writes an IR document to its standard
input, reads an IR document from its standard output, and reads diagnostics from
its standard error.

```
molto ──── IR (JSON) ───▸ molto-meson
      ◂─── IR (JSON) ────
      ◂─── diagnostics ──
```

Not a shared library. `dlopen` is faster and gives a plugin direct access to the
model, and it is wrong here for three reasons that compound: a stable C ABI
freezes struct layouts at 1.0 and every future field becomes a compatibility
negotiation; a plugin that corrupts the heap corrupts a build with no evidence
of who did it; and a plugin can only be written in a language with a C ABI,
which excludes most of the people who would write one. A process costs a spawn
and a serialisation, and buys a crash that is attributable, a language choice
that is free, and a schema that can grow (RFC-0013).

The contract:

- The plugin is invoked as `molto-<name> <capability>`, found in
  `~/.molto/plugins/bin` or, failing that, on `PATH`. The subcommand names the
  capability being asked for, so a plugin providing several does not have to
  infer which one from the document.
- Standard input is the IR document, and it is closed when the document ends. A
  plugin that reads to EOF has the whole request.
- Standard output **MUST** be an IR document and nothing else. Anything a plugin
  wants to say goes to standard error; a plugin that prints a banner to stdout
  has produced an unparseable document, and that is reported as such.
- Exit `0` means the document on stdout is the answer. Exit `3` means the plugin
  declines — the file is not one it understands — which is not an error and lets
  Molto try another frontend. Any other exit is a failure.
- A plugin that exceeds its **time limit** is killed, and a document larger than
  the **size limit** is refused mid-read. Both are reported as plugin failures.
  A plugin that hangs must not hang a build.
- A plugin that crashes fails its build and nothing else. There is no signal it
  can send that corrupts Molto's state, because it shares none.

Molto **MUST NOT** interpret a partial document. A plugin killed halfway leaves
valid JSON prefix and invalid meaning, and a build planned from half a
description is worse than no build.

## Permissions

A plugin's recipe lists what it may reach. The list is closed:

| Permission | Grants |
|---|---|
| `ir.read` | the document on stdin — implied by every capability |
| `ir.write` | returning a document |
| `project.read` | reading files under the project root |
| `project.write` | writing under the build directory, never under `src/` |
| `filesystem.project` | resolving paths under the project root and the global cache |
| `toolchain` | the resolved compiler and its version (RFC-0003) |
| `process.spawn` | executing a program named in a `BuildStep` |

There is no `arbitrary.network` and no `arbitrary.filesystem`, and their absence
is not an oversight. A frontend has no business making a network request: it is
translating a file that is already on disk, and a frontend that fetches
something has made the build depend on a server. A plugin that genuinely needs
bytes from elsewhere declares them as a dependency, where the lock file, the
checksum and the registry already handle it (RFC-0008).

**Denial is structural first, sandbox second.** A plugin has no network because
it is never handed a connection, no project files because it is handed a
document rather than a directory, and no toolchain because the paths are not in
the document unless `toolchain` was granted. The permission is the presence or
absence of the thing in what it receives. This is what "no handle, no
capability" means, and it is enforced by the shape of the interface rather than
by a check somebody might forget.

That covers what the plugin can *reach*, and it does not cover what it can
*ask Molto to do*. Where the operating system offers to close the gap, Molto
uses it: on Linux, a plugin without `process.spawn` is executed under a seccomp
filter that refuses `execve`, and one without `filesystem.project` under a
Landlock ruleset scoped to nothing. **Where the platform offers nothing, the
structural denial is what remains**, and this RFC says so rather than implying a
sandbox that is not there. Windows and macOS have no equivalent today, which is
one more reason both are deferred.

### The permission a plugin cannot be granted

Every permission above governs the plugin's process. None of them governs the
document it returns, and the document is executed by Molto, as the user, with
the user's privileges. A plugin denied everything can still return an IR whose
compile option loads a shared object into the compiler.

The answer is not here; it is in RFC-0013, which validates a document before any
of it becomes a command and applies stricter rules to a document whose origin is
a plugin. It is repeated here because the two halves are easy to mistake for
each other: **the sandbox decides what the plugin can touch, and the IR
validation decides what Molto will do on its behalf.** A design that has only
the first is a design that has none.

## Declaration and consent

Capabilities and permissions live in the recipe, which the registry serves as
parsed metadata (RFC-0010). This is deliberate: a client can read what a plugin
will ask for **before** downloading a byte of it, which is the same reason a
dependency's interface is served that way.

`molto plugin install` shows the capabilities and permissions and asks for
confirmation. A plugin whose new version asks for more than the installed one
asks again — an upgrade that silently widens what a binary may do is the shape
of every supply-chain incident worth naming.

**This requires an exception to a rule in RFC-0009.** That RFC says unknown keys
in a recipe are ignored, so a reader is not broken by a format that grew. Here,
a reader that ignores a `[plugin]` table it does not understand runs an
executable under permissions it never saw. It is the only place in the ecosystem
where "ignore what you don't know" is actively dangerous, and the fix is that
`[tool].kind = "plugin"` raises the recipe's `schema`: a Molto that does not
know the plugin schema refuses the recipe rather than reading a subset of it.

**Ownership is a precondition, not a caveat.** RFC-0010 calls the absence of
package ownership "the most serious gap in this document": any authenticated
account may publish any name, with no owner, no reservation and no defence
against a typo publishing under someone else's name. That gap, applied to
libraries, produces a bad dependency. Applied to executables Molto downloads and
runs, it produces a bad executable Molto downloads and runs. **No plugin may be
served from the official registry until ownership exists.** Local plugins and
private registries are unaffected; the public distribution path is what waits.

## Distribution

A plugin is `kind = "tool"` and travels the protocol of RFC-0010 with no
changes: the catalogue reads, the download with its checksum and immutable cache
headers, the two-step publish. There is no `/v2`.

Two things are missing and both are small:

- **`[tool].kind` does not admit `plugin`.** RFC-0009 enumerates `formatter`,
  `linter` and `linker`, and the implementation is narrower still — the tool
  kinds in the tree are formatter and linter, with `linker` specified and never
  built. Adding `plugin` touches the registry's recipe validation and that
  enumeration.
- **`[plugin]` does not exist**, and it is where capabilities, extensions,
  permissions, `ir_schema` and `molto_min` go.

Installed plugins live in `~/.molto/plugins/`, with binaries under `bin/` and
the recipe kept beside each one, because the permissions a plugin was installed
under have to be readable later without asking the registry. A plugin is not a
project dependency: it is not in the lock file, does not affect resolution, and
two projects on one machine share the installed copy.

## Diagnostics from a plugin

A plugin does not invent a diagnostic format. There are two, both already
specified, and a plugin uses whichever is closer to what it has:

- A plugin relaying a tool's output writes the **one-line form** of RFC-0011 —
  `file:line:col: severity: message [rule]` — which Molto's parser accepts by
  design and never fails on.
- A plugin producing structured findings writes the **JSON form** that
  `molto lint --format json` already emits.

The rules that apply either way:

- **The column model is declared**, not assumed. RFC-0011 specifies it because a
  caret drawn with a byte offset over a UTF-8 line points at the wrong column,
  and a plugin that omits it inherits the gcc default whether or not that is
  what it meant.
- **Severity is one of the four that exist.** `unknown` is the landing zone for
  anything that does not parse, and it is reprinted verbatim rather than
  reshaped into a frame that would misplace it.
- **A plugin does not invent rule identifiers in Molto's namespace.** RFC-0011
  is explicit that there is deliberately no Molto error-code space; a plugin's
  diagnostic carries the underlying tool's rule, or none.
- The footer gains a line that does not exist today. RFC-0011 defines
  `= dependency:`, `= source:` and `= compiler:`, and a plugin's diagnostic has
  no compiler. It carries `= plugin: <name> <version>` instead, so that the
  question "what produced this message" has an answer for every message.
- The per-tool, per-file output limit of RFC-0011 applies to a plugin exactly as
  it applies to clang-tidy. Truncation is correctness, not tidiness.

## Naming a plugin

A project names its plugins in `[plugins]`:

```toml
[plugins]
closures = "1.2.0"
```

Exact versions, always — the rule is RFC-0008's and it is not relaxed for
something that executes.

**`[plugins]` fails closed.** This is not the default behaviour of the manifest
parser: today every table except `[package]` drops unknown keys in silence, and
an unknown table is ignored entirely. A `Project.toml` naming a plugin, read by
a Molto that predates plugins, would therefore build the project *without the
plugin*, successfully, and say nothing — a green build of the wrong thing, which
is the failure mode this ecosystem is arranged to prevent. RFC-0003 already
made `[package]` fail closed for exactly this reason.

Failing closed on a table an old reader does not know requires the old reader to
know it should fail, which it cannot. So `[plugins]` forces the manifest schema
key RFC-0003 reserved: a manifest that names plugins declares a schema, and a
Molto that does not understand that schema refuses the manifest rather than
reading half of it.

**`[plugins]` and `[build-deps]` are not the same table**, and RFC-0003 reserves
the second as "dependencies needed to run a build rather than to link into it",
which describes a plugin exactly. The distinction is that a `[build-deps]` entry
is a package, resolved by the resolver, recorded in the lock file and built from
source; a `[plugins]` entry is an installed executable with declared permissions
that Molto runs. They may share a resolver one day. They do not share a
consent model, and merging them would put "this binary may spawn processes"
inside a list nobody reads that carefully.

## CLI

| Command | Does |
|---|---|
| `molto plugin list` | what is installed, with capabilities and permissions |
| `molto plugin info <name>` | one plugin in full: where it came from, what it may do |
| `molto plugin install <name>[@<version>]` | fetch, verify, show permissions, confirm |
| `molto plugin remove <name>` | uninstall |

`list` and `info` show permissions rather than hiding them behind a flag,
because the entire security posture of this design is a user's ability to answer
"what is installed and what may it do" without reading a recipe by hand.

`molto <name>`, for a plugin providing `command`, dispatches to it. The CLI
today reports an unknown command as a usage error and does not look further;
that lookup is where dispatch belongs, and it is checked **after** the built-in
table, never before, so no plugin can shadow `build`.

`molto migrate` is reframed rather than added. RFC-0002 specifies it as an
importer for Make, CMake and Meson projects, and it has never been implemented.
Written now it would be a second parser for the same files a compatibility
frontend parses, and the two would drift. `migrate` therefore **runs a frontend
once and serialises the resulting IR to a `Project.toml`**: one parser, two
products — a permanent conversion and a continuous translation.

One exit code is added: **`6`, plugin failure** — a crash, a timeout, a document
that fails validation, or a permission that was refused. RFC-0002's own
reasoning applies: the codes exist so a script can tell "this broke" from "this
does not exist yet", and a script equally needs to tell "my code does not
compile" from "a third-party binary misbehaved". The existing codes cover the
rest: a manifest naming an unknown plugin is `2`, and a plugin that cannot be
resolved from a registry is `3`.

## Implementation Status

Nothing is implemented. `spec.md` §14 is five lines and this RFC is the first
document to specify any of it.

The order that matters, because parts of it are cheap and parts are not:

- **The CLI fallback** — an unknown command looking for `molto-<name>` — is the
  smallest piece and delivers `command` plugins on its own.
- **`molto plugin install/list/info/remove`**, which needs `[tool].kind =
  "plugin"` and the `[plugin]` table in the registry's validation first.
- **Process execution and the IR exchange**, which waits on RFC-0013.
- **Sandboxing**: seccomp and Landlock on Linux, and nothing anywhere else.
  Until it exists, permissions are enforced structurally only, and
  `molto plugin info` **MUST** say so rather than implying a boundary that is
  not there.
- **The Meson frontend**, which is a separate RFC and the first real test of
  whether the subset above is a language anyone can build with.
- **Ownership in the registry**, which gates public distribution entirely.

## Non-Goals

A plugin is not a build script. It does not run because a dependency was
resolved, is not carried inside a package, and cannot be introduced by anything
a project depends on. RFC-0009 refuses hooks in recipes on the grounds that they
would make every dependency a remote code execution with extra steps, and
nothing here weakens that: a plugin is installed deliberately, by the person who
owns the machine, and named in a manifest they wrote.

A plugin is not a compiler. `compiler` as a capability means a plugin may drive
one for a language Molto's toolchain does not resolve — that is what a CUDA
target needs — and it does not mean Molto has opinions about code generation.
The project's stated non-goal stands (RFC-0001).

A plugin does not extend the CLI's built-in commands. It adds new ones; it
cannot add a flag to `build`, wrap `test`, or change what an existing command
means. A command whose behaviour depends on what is installed is a command whose
documentation is wrong on somebody's machine.

A plugin does not participate in dependency resolution. Resolution is
RFC-0008's, on the client, over a lock file, and a plugin that could influence
version selection would make a lock file a suggestion.

There is no stable C ABI, and there will not be one at 1.0. `spec.md` §23 lists
"Stable Plugin ABI" as a 1.0 deliverable, and this RFC reinterprets it as a
stable *contract*: the IR schema, the process protocol and the permission
vocabulary. That is the promise worth freezing.

## Reserved / Future

- **WASM as an execution model.** It is the honest answer to sandboxing: WASI is
  a capability system by construction, deny-by-default is real rather than
  structural, and it works identically on Windows and macOS, where the syscall
  filters above do not exist. It waits because embedding a runtime is a
  dependency in a tree that links none, and because the process protocol has to
  be proven first. A future revision could keep every word of the permission
  table and change only how it is enforced.
- **Signing.** A checksum proves bytes did not change in transit; a signature
  proves who produced them, and it matters more for something Molto executes
  than for something it links. It follows ownership.
- **Plugin-provided diagnostics with fix-its**, once the diagnostic model has
  them.
- **A `phase` for a packager**, which needs somewhere in the build viewport to
  appear (RFC-0015).
- **Capability negotiation** — a plugin advertising several IR schema versions
  rather than one — if the schema turns out to move faster than plugins do.
- **Plugins as workspace members**, so a project can develop its own frontend
  without publishing it.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md) — extensible architecture, and the compiler Molto is not
- [RFC-0002: CLI Specification](0002-cli-specification.md) — `molto plugin`, the reframed `migrate`, and exit code 6
- [RFC-0003: Project Manifest](0003-project-manifest.md) — `[plugins]`, the schema key it forces, and `[build-deps]` beside it
- [RFC-0007: Build System](0007-build-system.md) — the Non-Goal that named a plugin as the answer
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — `[tool].kind`, the `[plugin]` table, and the unknown-key rule this RFC carves an exception in
- [RFC-0010: Registry Specification](0010-registry-specification.md) — how a plugin is served, and the ownership gap that gates it
- [RFC-0011: Build Diagnostics](0011-build-diagnostics.md) — the two formats a plugin may write, and the footer line it adds
- [RFC-0013: Build Intermediate Representation](0013-build-intermediate-representation.md) — what a plugin reads and writes, and what the engine refuses to lower
- [RFC-0015: Build Pipeline and Transforms](0015-build-pipeline.md) — where in a build each capability runs

See also `spec.md` sections 14 (Plugins) and 23 (Roadmap).
