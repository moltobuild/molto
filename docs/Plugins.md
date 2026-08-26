# Writing a frontend plugin

A **frontend** teaches Molto to read a project it does not understand. Point it
at a directory holding a `meson.build`, a `CMakeLists.txt` or a `Makefile`, and
it returns a description of what to build.

This page is for someone writing one. It says what the contract is, what a
frontend may do, and — the longer half — what it may not, and why each refusal
is deliberate rather than a gap somebody will fill in later.

```sh
molto ir                    # the document for this directory, to stdout
molto ir -o out.json        # to a file
molto ir -p release         # with the release profile's options folded in
```

`molto ir` is how you develop and test one. It runs the frontend and stops
before the engine, so what it prints is exactly what your plugin returned, after
Molto checked it.

## The shape of it

A plugin is an executable named `molto-<name>` and a recipe beside it. Molto
runs `molto-<name> frontend`, writes a request to its standard input, and reads
a document from its standard output.

```
molto ──── request (JSON) ───▸ molto-meson frontend
      ◂─── IR document (JSON) ──
      ◂─── diagnostics ─────────  (stderr, straight to the user)
```

Not a shared library. A process costs a spawn and a serialisation, and buys a
crash that is attributable, a language choice that is free — anything that can
read stdin and write stdout is a plugin, including a shell script — and a schema
that can grow without freezing struct layouts at 1.0.

### What you receive

```json
{"schema": 2, "request": "frontend", "root": "/w/app", "entry": "meson.build"}
```

`root` is absolute, and every relative path in your answer is anchored at it.
`entry` is the filename from your own `extensions` that made you the candidate,
so a plugin that reads several kinds of build file does not have to go looking.

Read standard input to EOF: Molto closes it when the request ends, so reading to
EOF is how you know you have all of it.

### What you return

An IR document, and nothing else, on standard output.

```json
{
  "schema": 2,
  "files_read": ["meson.build", "src/meson.build"],
  "projects": [{
    "name": "app",
    "version": "0.1.0",
    "root": "/w/app",
    "origin": "meson",
    "targets": [{
      "name": "app",
      "kind": "executable",
      "sources": [{"path": "src/main.c", "language": "c", "options": []}],
      "options": [{"value": "-DFEATURE=1", "scope": "target"}],
      "includes": [{"value": "include", "scope": "target", "system": false}],
      "links": [{"value": "m", "scope": "target"}],
      "depends_on": [],
      "artifact": {"kind": "executable", "path": "app"}
    }],
    "dependencies": [{
      "name": "greet",
      "version": "1.2.0",
      "origin": "registry",
      "scope": "runtime",
      "root": "/home/you/.molto/cache/greet-1.2.0",
      "interface": {
        "includes": [{"value": "include", "scope": "target", "system": false}],
        "options": [],
        "links": []
      }
    }]
  }]
}
```

Four fields carry more weight than they look like they do:

- **`origin` must be your plugin's own name.** Not `native`, which is reserved
  for `Project.toml`. Molto checks it and refuses the document otherwise, and
  the reason is the next section: `native` gets a looser set of rules, and a
  plugin that could claim it would be claiming those rules.
- **`files_read` must list every file you opened.** All of them. A frontend that
  reads `meson.build` and four `subdir()` files and reports only the first has
  described a project that will not be re-read when the other four change. A
  document reporting nothing read is refused.
- **`projects` is an array with exactly one element.** One project per document
  in this revision. The array is an array so that the day workspaces are
  specified the schema widens without a new revision.
- **A dependency's `scope` says which targets may compile against it.**
  `runtime` reaches every target; `dev` reaches the test targets and no others,
  and that is what makes RFC-0008's separation real rather than documented — a
  source under `src/` that includes a development dependency fails to compile.
  It is required rather than defaulted to `runtime`, because a missing scope and
  a runtime scope would be the same document and only one of them is safe.

### Exit codes

| Exit | Means |
|---|---|
| `0` | the document on stdout is the answer |
| `3` | you decline — this is not a file you understand; Molto tries the next candidate |
| anything else | you failed, and the build fails with exit code 6 |

Exit 3 is not an error and it is worth using. A plugin that reads `CMakeLists.txt`
but only the subset it implements should decline a file it cannot handle rather
than return half a description.

Anything you want to say goes to **standard error**, which reaches the user
directly. A banner on stdout is an unparseable document, and it is reported as
one.

### The recipe

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
permissions = ["ir.write", "project.read"]
ir_schema = 2
molto_min = "0.21.0"
```

`extensions` are **filenames, not suffixes**: Molto checks whether that file
exists at the root, and that is what makes you a candidate.

`ir_schema` and `molto_min` are checked **before your process starts**. A
mismatch found there is a refusal; found halfway through a document it would be
a half-read document.

## What a frontend can do

- Read any file under the root it was pointed at, and interpret it however it
  likes. It is your parser and your language.
- Describe as many targets as the project has, of kind `executable`, `static`,
  `shared`, `object` or `test`, with dependencies between them via `depends_on`.
- Attach compile options, include paths and link options at `target`, `profile`
  or `unit` scope.
- Decline, with exit 3.
- Write diagnostics to stderr, in either of the two formats Molto already
  parses: the one-line form `file:line:col: severity: message [rule]`, or the
  JSON that `molto lint --format json` emits.
- Be written in anything. It is a process.

Note that `static` and `shared` are **expressible but not yet buildable**. The
IR carries the node and the engine reports it cannot build it. That is on
purpose: a frontend for Meson, whose whole vocabulary is libraries, has to be
writable before Molto grows shared library support.

## What a frontend cannot do

### It does not run the tool it understands

A Meson frontend does not invoke `meson`. It reads `meson.build` and interprets
it.

This is the single most consequential rule here, and it costs real work, so the
reasoning should be legible. Invoking the real tool is easier, more complete and
correct by construction — and it hands control of the build to a file in the
dependency, which can run anything, on the machine of anyone who builds it. A
`meson.build` can call `run_command`; a `CMakeLists.txt` can call
`execute_process`. Delegating to an installed build system *is* remote code
execution with extra steps.

So the subset you implement is chosen so that it cannot. For Meson, that is
roughly: `project`, `executable`, `library`, `shared_library`, `static_library`,
`files`, `dependency`, `include_directories`, `declare_dependency`,
`configure_file`, `if`, `foreach`, variables and user-defined functions.

Note the absences, and note that the absences are the point. No `run_command`.
No `find_program` reaching into an arbitrary `PATH`. No `import('python')`. No
module system. No `get_option` reading an environment Molto does not control.

**A construct you do not implement is a rejected build file with the line
reported — never a fallback to the real tool.** Falling back means the safe path
silently becomes the unsafe one at the moment a project gets complicated enough
to need it.

The consequence is that some projects will not build. A `meson.build` that shells
out to a script is outside this subset and will stay outside it. That is a real
limitation and it is the correct one: the alternative is that Molto builds
everything, including the thing that erases a home directory.

### It gets no network

There is no `arbitrary.network` permission and its absence is not an oversight.
A frontend is translating a file that is already on disk, and one that fetches
something has made the build depend on a server. A plugin that genuinely needs
bytes from elsewhere declares them as a dependency, where the lock file, the
checksum and the registry already handle it.

### It writes nothing

A frontend describes. It does not produce.

### It cannot emit a node type this schema does not carry

Two exist in the specification and are not in schema 2, and both are refused **by
name** so the message tells you which:

- **`BuildStep`** — a command that is not a compile and not a link. This is what
  a Meson `custom_target` becomes, and it needs the `generator` capability,
  which nothing grants yet. A document carrying `steps` is refused.
- **`GeneratedSource`** — a source that does not exist until something ran. It is
  a `Source` carrying `produced_by` and `deterministic`, and a document carrying
  either is refused. It needs a `BuildStep` to produce it.

More generally: **an unknown attribute on a known node is ignored, and an unknown
node type is fatal.** Adding `"license": "MIT"` to a project is harmless and will
be skipped. Adding `"toolchains": [...]` is refused. The asymmetry is the whole
point — an attribute refines work that is already described, and a node type is
work that is not described at all, so an engine that skipped one would build
something other than what it was handed and report success.

The same applies to vocabulary. A `kind` of `framework`, a `language` of `rust`,
a `scope` of `everywhere`, a dependency `origin` of `ftp` — each is refused
rather than defaulted, because each names what a node *is*.

### Its paths stay inside three directories

A target's paths are relative to `Project.root`, unless the target names a
`package` — then they are relative to that `Dependency`'s `root`. Naming a
package the document does not describe is refused; so is naming one and then
climbing out of its root. The anchor moves, the fence does not.

Every path in your document — a source, an include, a dependency root — must
resolve inside the workspace root, the profile's build directory, or the global
cache. By `..`, by an absolute prefix, or through a symlink alike. An artifact's
path must be inside the build directory specifically: a target writing into
`src/` is editing the user's code as a side effect of a build.

Three and not four: RFC-0013 has a fourth bound — the directories the user's
manifest authorised — and your document is validated before anything has been
resolved, so there are none to authorise. Nothing you write can add one.

### Some options are refused from a plugin and not from `Project.toml`

| Refused | Why |
|---|---|
| `-fplugin`, `-fplugin-arg-*`, `-load`, `-Xclang` | they load code into the compiler — a second extension mechanism arrived at sideways, with none of these rules |
| `-B`, `--sysroot`, `-isysroot`, `-fuse-ld` | they redirect the toolchain, and the toolchain is `pickup`'s answer rather than a frontend's opinion |
| `-o`, `--output` | the engine composes output paths; naming one is describing where your object goes, which is not your decision |

**This is the asymmetry to understand, and it is not a statement about trust.**
`Project.toml` is a file in the user's repository, which they wrote, which their
reviewer read and their version control records. Your document is generated on
the fly by a binary fetched from a registry. Those two deserve different
scrutiny even when the second is entirely well-behaved, and the day they do not
is the day the first one stopped being reviewable.

There is a second half worth stating plainly, because the two are easy to
mistake for each other. Permissions govern **your process**; they do not govern
the document it returns, and the document is executed by Molto, as the user,
with the user's privileges. A plugin denied everything can still return a
compile option that loads a shared object into the compiler — which is exactly
why the table above exists. **The sandbox decides what a plugin can touch; the
document check decides what Molto will do on its behalf.** A design with only
the first has neither.

Speaking of which: **there is no sandbox yet.** Permissions are enforced
structurally — you have no network because you are never handed a connection,
and no project files because you are handed a document rather than a directory —
and `molto plugin info` says so rather than implying a boundary that is not
there. Seccomp and Landlock on Linux are the plan.

### It has a deadline and a size limit

Thirty seconds, and sixteen megabytes. A plugin that exceeds either is killed or
refused mid-read. A plugin that hangs must not hang a build. Both are Molto's to
choose rather than yours: a limit a plugin could raise is not a limit.

### It cannot take over a directory Molto already understands

`Project.toml` is checked first, always. If there is one, the native frontend
answers and no plugin is consulted — the same rule the CLI applies to command
names, and for the same reason.

## Testing one

`molto ir` is the whole loop. Write the plugin, put it (or a symlink to it) in
`~/.molto/plugins/bin/molto-<name>` with a recipe in
`~/.molto/plugins/recipes/<name>.toml`, and run `molto ir` in a directory your
extension matches.

Two runs over one project produce one **byte-identical** document. That is a
guarantee you can lean on: a fixture is a file, and a test is a `diff`. It also
means your own frontend should not emit anything order-dependent — sort what
comes off a directory walk, or your document differs between machines and its
fixture is useless.

## What is not built yet

This is the first revision of the frontend capability, and it stops in a
deliberate place.

- **`molto build` does not yet build from a plugin's document.** It builds from
  a *document* now — the native frontend's — and takes from it what is compiled:
  the targets, their sources and each source's language. What it still takes
  from `Project.toml` directly is the compile line, and it asks the native
  frontend by name rather than asking whichever frontend understands the
  directory. So a plugin's document is still produced, inspected and validated
  rather than built, which is what a frontend author needs in order to write one
  at all, and the contract has to be stable before the engine depends on it.
- **`molto migrate` is not implemented.** It is specified as running a frontend
  once and serialising the result to a `Project.toml` — one parser, two products.
- **A frontend still does not resolve.** Dependencies reach the document from
  transforms that run after `resolve`, not from the frontend, so the document a
  plugin returns reports an empty `dependencies` list and the engine fills it —
  along with one `Target` of kind `object` per package that ships sources. This
  is deliberate: a frontend that resolved would make `molto ir` touch the
  network.
- **No sandbox**, as above.

## Where the rules come from

| Question | Read |
|---|---|
| What a plugin is, the eight capabilities, permissions, distribution | `rfcs/0014-plugin-system.md` |
| The document: every node, the schema rule, what the engine refuses to lower | `rfcs/0013-build-intermediate-representation.md` |
| The two diagnostic formats a plugin may write | `rfcs/0011-build-diagnostics.md` |
| `recipe.toml`, `[tool].kind`, the unknown-key rule this carves an exception in | `rfcs/0009-recipe-specification.md` |
| Where in a build each capability runs | `rfcs/0015-build-pipeline.md` |
