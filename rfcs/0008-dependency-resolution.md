# RFC 0008: Dependency Resolution

- RFC Number: 0008
- Title: Dependency Resolution
- Status: Draft
- Created: 2026-08-07

## Summary

This RFC specifies how Molto turns the dependency tables of a manifest into a
concrete, reproducible set of dependencies on disk: what each of the five
sources means, why every version is exact, how a conflict between two exact
versions is found and settled, what separates a development dependency from one
that ships, what is written to the lock file, and what is cached.

It does **not** redefine `[deps]`. RFC-0003 already fixes the format, the keys
and their validity rules, and that table is the input to this document rather
than part of it. What RFC-0003 left open is the algorithm, and it also reserved
`[dev-deps]`, which this RFC un-reserves and specifies.

## Motivation

`[deps]` is decorative today. A manifest can declare dependencies and nothing
happens: the manifest reader binds `[package]`, `[target]`, `[test]`, `[env]`
and `[profile.*]`, and never looks at `deps` or `[registries]`. `molto add`,
`molto remove` and `molto update` exit with the "not implemented" code that
RFC-0002 reserves for exactly this.

Worse, the canonical form is unreadable. The manifest parser recognises an
inline table and **discards the key in silence**, so
`http = { path = "modules/http" }` parses successfully and yields nothing. The
multi-line form shown in `spec.md` §7 fails differently and more confusingly:
line gathering only counts brackets, never braces, so `sqlite = {` reaches the
inline-table branch on its own and is dropped; the lines under it are then
parsed as ordinary keys of `[deps]`; and the manifest finally fails three lines
later, on the closing `}`, with `expected '='`. A specification that ignores
this would be specifying an algorithm whose input cannot be read.

Everything else in Molto is blocked behind this. There is no reason to build a
global artifact cache (RFC-0007) with nothing to put in it, no reason for the
build graph to grow edges with no packages to order, and no reason for a
registry client to read a catalogue nobody consumes.

## Precondition: the manifest must be readable

Two gaps in `src/util/toml.c` are prerequisites, not implementation detail, and
they belong in this RFC because they define the boundary of its input:

- **Inline tables must parse.** `{ git = "…", tag = "3.50.0" }` is the primary
  form of a dependency in RFC-0003. Skipping it silently is the failure mode
  this whole ecosystem is meant to avoid: the user wrote a dependency, Molto
  read the file without complaint, and the dependency is not there. Until inline
  tables parse, an unparsed one **MUST** be an error rather than a skip.
- **The manifest must be writable.** `molto add` and `molto remove` modify
  `Project.toml` (RFC-0002), and there is no TOML writer. The writer must
  preserve comments, key order and formatting outside the table it edits;
  RFC-0003 promises the file is never "generated or overwritten silently", and
  a writer that reformats a hand-edited manifest breaks that promise even when
  the semantics survive.

Reading unknown keys of a table is already possible — the parser exposes the
declaration-ordered key list that a `[deps]` reader needs — so the shape of
`[deps]` is not the problem. The values are.

## The five sources

Every dependency has exactly one source (RFC-0003). What follows is what each
one fetches and what makes it deterministic.

### `version` — the registry

The default. A name and one exact version, fetched from a registry (RFC-0010)
and verified against the checksum the registry reports. There is nothing to
select: the version in the manifest is the version that is fetched.
`registry = "<name>"` picks a registry declared in `[registries]`; without it,
the official one is used.

### `git`

A repository URL, paired with at most one of `branch`, `tag` or `rev`. All three
resolve to a **commit id**, and it is the commit id that is recorded: a branch
is not a version, and `main` today is not `main` next week. A dependency
declared by branch is therefore reproducible for everyone who shares the lock
file, and moves only when `molto update` is run.

Molto clones; it does not vendor. The working copy lives outside the project and
is never committed.

### `path`

A local directory, relative to the manifest that declares it. It is the source
for a dependency being developed alongside its consumer, and it is the one
source with no version and no integrity check — the bytes are whatever is on
disk right now, which is the point.

A path dependency is **never cached and never published**. A package whose
`[deps]` contains a `path` entry cannot be published to a registry, because the
path means nothing on another machine.

### `archive`

A URL to a source archive. Because a URL can serve different bytes tomorrow, an
archive dependency **MUST** carry a checksum; without one there is no difference
between "the upstream re-rolled the tarball" and "someone replaced it".

### `recipe`

A recipe name resolved through a registry. The recipe describes how the
dependency is obtained and built (RFC-0009); this source exists for the
libraries that were never designed to be consumed as packages — the ones with
a `configure` script and thirty years of history. The recipe is the adapter.

## Versions are exact, always

A dependency names one version. `sqlite = "3.50.0"` means 3.50.0 — not the
newest 3.x, not "at least this". There are no ranges in a Molto manifest:

> The operators `^`, `~`, `>=`, `>`, `<`, `<=`, `*` and comma-separated
> conjunctions are **not part of the format**. A version string that is not a
> single concrete version is a manifest error.

This is the most consequential decision in this RFC and it is a security
decision, not an ergonomic one.

A range is a standing authorisation to run code that does not exist yet, granted
to whoever controls the publisher's account at some point in the future. That
is not a hypothetical failure mode; it is how supply-chain compromises actually
propagate. The pattern repeats: an account is taken over, a patch release is
published, and it reaches thousands of builds within hours — not because anyone
adopted it, but because nobody had to. No commit was made, no review happened,
and no diff exists to point at afterwards. The range did the adopting.

An exact version does not prevent a malicious release from being published. It
prevents it from arriving unasked. Someone has to write the new number, and
writing it produces a diff that a human can be asked to approve.

The standard objection is that pinning keeps security patches out. It does, and
that is the trade being made deliberately: a patch that lands without anyone
looking at it is not a review, it is a download. Molto's answer is to make
updating cheap and visible rather than automatic and invisible — `molto update`
proposes the newer versions and rewrites the manifest, so the upgrade is one
command and one reviewable diff.

Semver still matters, for two things and not for a third. It **orders**
versions, so Molto can tell which release is newer and propose the right one. It
**validates** them, so a typo is caught rather than compared byte by byte. It
does not gate resolution, because there is nothing to satisfy: there are no
ranges, so there is no constraint solver, no backtracking and no SAT problem
hiding in this document.

`molto add sqlite` resolves the newest version and **writes that exact number**
into `Project.toml`. `molto add sqlite@3.1.2` writes the one asked for. Either
way the manifest ends up naming a version, because a manifest that does not is
not expressible.

Version comparison lives in Molto, not in the registry. The registry treats a
version as an opaque string and serves exact coordinates only (RFC-0010), which
keeps the protocol free of a semver dialect and lets a private registry be
implemented by anyone who can serve JSON.

## One version per dependency

This is the decision that separates a C package manager from a Rust one.

Cargo resolves conflicting requirements by keeping both: `serde 1.4` and
`serde 2.0` coexist in the same binary because Rust mangles each crate's symbols
with a distinct hash, so the two never collide. That mechanism does not exist
here. The C linker has one flat namespace. Two copies of the same library in
one link produce duplicate symbols if you are lucky, and if you are not, they
produce a binary in which allocations from one copy are freed by the other and
every struct layout is a coin flip.

Therefore:

> **A dependency appears exactly once in a resolved graph.** Two dependents that
> name different versions of the same package are in conflict, and the conflict
> is resolved before anything is downloaded.

With exact versions, unification is equality. Either every dependent named the
same version or they did not; there is no interval to intersect and no
cleverness available. What replaces the interval arithmetic is a search, and
then a question.

Molto does not resolve this by vendoring, renaming or namespacing. Each of those
is a real technique and each of them is a compiler feature Molto does not have,
which puts them outside the boundary of RFC-0001.

## Conflicts are found before anything is fetched

A recipe declares its own `[deps]` (RFC-0009), and the registry serves recipes
without serving archives (RFC-0010). The whole graph is therefore walkable over
metadata alone: Molto knows what `sqlite 3.50.0` depends on without downloading
a single byte of it.

That is what makes the following possible, and it is why the conflict is a
question asked during resolution rather than an error discovered at link time.

**1. Detect.** Walking the metadata graph yields, for each package, the set of
versions its dependents named. A set with more than one member is a conflict.

**2. Propose.** Molto searches for a version that removes it: for each candidate
release of the conflicting package, and of the dependents that pull it in, it
re-walks the metadata graph and checks whether a conflict-free assignment
exists. It proposes the **newest** combination that works. The search is bounded
by what the registry publishes and needs no archives, so it costs requests
rather than downloads — but it costs enough of them to be worth reporting, so it
draws the four-frame spinner (`-` `\` `|` `/`) on stderr while it runs, the same
one pickup shows while the registry answers.

**3. Ask.** The proposal is presented and the user decides:

```
molto: png is required at two versions
    1.6.40  ← required by libspng 0.7.4
    1.5.30  ← required by cairo 1.18.0, which you depend on directly

  Upgrading cairo to 1.18.2 requires png 1.6.40 and resolves this.

  Apply? [Y/n]
```

Accepting writes the new version into `Project.toml`. That is the point: the
resolution ends up in the manifest, in the diff, and in review — not hidden in
the lock file where nobody reads it.

**4. Refuse to guess when nobody is there.** Without a terminal on standard
input, Molto does not prompt. It prints the same conflict and the same proposal
and exits with code 3 (RFC-0002). A CI run that silently accepted a version
nobody chose would reintroduce exactly the behaviour the exact-version rule
exists to prevent. This follows the precedent already set by `molto login`,
which refuses to read a password that is not being typed at a terminal.

If the search finds nothing, step 2 says so and the message is the same minus
the proposal. There is always an action available to the user — change a version
they declared, or stop depending on one of the two packages — and the message's
job is to make clear which versions are in play and who asked for each.

## `[dev-deps]`: what ships and what does not

RFC-0003 reserves `[dev-deps]`. This RFC un-reserves it, because "does this
dependency end up in the binary I ship?" is not a question that can wait for a
later revision — it is the question a user asks the first time they add a test
framework.

A dependency's scope is the table it is declared in. `[deps]` is linked into the
package's binary and is seen by everything it is published with. `[dev-deps]` is
for the code that only exists while the package is being developed: test
frameworks, benchmark harnesses, fixture generators.

### The separation is enforced with `-I`, not with documentation

A dev dependency's include directories are added to the command line that
compiles `tests/`, and to no other. They are absent from the one that compiles
`src/`.

The consequence is the property that makes this real: a source under `src/` that
includes a dev dependency's header **fails to compile**, with `fatal error: no
such file or directory`, on the first build. Not at link time, not at package
time, and not in production. Nothing has to remember the rule, because the rule
is the absence of a flag.

### Dev dependencies are not transitive

Only the root package's `[dev-deps]` are resolved. The `[dev-deps]` of a
dependency are read and ignored — deliberately, and worth a comment wherever
that happens in the code.

If they were transitive, adding one small library would drag in its test
framework, its mocking library and its benchmark harness, none of which will
ever be compiled and all of which would have to be downloaded, resolved, and
conflict-checked against everything else. Cargo makes the same choice for the
same reason.

### But they are unified with the runtime ones

This is where Molto and Cargo part company again, and for the reason given
above about the linker.

A test binary links the objects of `src/` — compiled against the **runtime**
version of a package — together with the objects of `tests/`, compiled against
the **dev** version. If those are two different versions of the same library,
that link has duplicate symbols or a silent ODR violation, which is the exact
failure the one-version rule exists to prevent.

So the two tables share one graph and one version per name. Declaring
`png = "1.6.40"` under `[deps]` and `png = "1.5.30"` under `[dev-deps]` is a
conflict, and it is reported like any other.

### In the lock and on publication

Every locked package carries a `scopes` array recording which tables pulled it
in, so a production build can fetch only what it links, and `molto test` can
fetch the rest.

Publishing a package does not publish its dev dependencies. A consumer resolving
your package sees its `[deps]` and never learns which test framework you used,
which is as it should be.

## The lock file

Resolution writes `Molto.lock` at the workspace root. It records, for every
package in the resolved graph, the exact version, the exact source (including
the resolved commit id for a `git` dependency), the integrity checksum, and the
dependents that pulled it in.

`Molto.lock` is **generated and committed**. It is generated because a
hand-edited lock file is a lock file that lies. It is committed because that is
the only thing that makes a build reproducible on a second machine.

The obvious alternative is the WSDB, and it is wrong. The WSDB is local,
disposable, and `.gitignore`d by design (RFC-0004) — deleting it must never do
more than force a rebuild. A resolution stored there would be re-derived on
every clone, on every CI run, and on every teammate's machine, which is to say
it would not be a resolution at all but a fresh guess each time, taken against
whatever the registry happens to serve that day. Reproducibility is a stated
goal, and it is a property of a file that travels.

### Format

TOML with an array of tables, which the parser already reads. Two keys at the
root:

| Key | Type | Description |
|---|---|---|
| `version` | integer | Lock format version. `1` for this document |
| `root` | string | Name of the package this lock belongs to |

Then one `[[package]]` per resolved package:

| Key | Type | Required | Description |
|---|---|---|---|
| `name` | string | yes | Package name |
| `version` | string | yes | The exact resolved version |
| `source` | string | yes | Prefixed origin; see below |
| `checksum` | string | no | `sha256:<64 hex>` of the artifact. Absent only for `path` |
| `scopes` | array[string] | yes | `runtime`, `dev`, or both |
| `dependencies` | array[string] | yes | Names of this package's direct dependencies |

`source` is a single string with a scheme prefix, so one key answers both "where
did this come from" and "which fetcher gets it":

| Form | Example |
|---|---|
| `registry+<url>` | `registry+https://molto-registry.example.dev` |
| `git+<url>#<rev>` | `git+https://github.com/sqlite/sqlite#5a1e8ff…` |
| `archive+<url>` | `archive+https://example.dev/png-1.6.40.tar.gz` |
| `recipe+<url>#<name>` | `recipe+https://molto-registry.example.dev#libpng` |
| `path+<relative>` | `path+modules/http` |

A `git` source records the **resolved commit**, never the branch or tag it was
written as: that is what makes a branch dependency reproducible. A `path` source
carries no checksum, because its bytes are whatever is on disk — which is the
point of a path dependency and the reason it cannot be published.

```toml
version = 1
root = "my_app"

[[package]]
name = "http"
version = "0.2.0"
source = "path+modules/http"
scopes = ["runtime"]
dependencies = ["yyjson"]

[[package]]
name = "moltest"
version = "0.4.1"
source = "registry+https://molto-registry.example.dev"
checksum = "sha256:3f786850e387550fdab836ed7e6dc881de23001b3f786850e387550fdab836ed"
scopes = ["dev"]
dependencies = []

[[package]]
name = "yyjson"
version = "0.10.0"
source = "registry+https://molto-registry.example.dev"
checksum = "sha256:8f14e45fceea167a5a36dedd4bea2543f14e45fceea167a5a36dedd4bea25438"
scopes = ["runtime"]
dependencies = []
```

Packages are written **sorted by name**, and every array preserves the order it
was resolved in. A lock file whose diff reorders itself between runs is a lock
file nobody reads, and the whole value of committing it is that its diff is
worth looking at.

There is deliberately no `requested_by` key: it is the `dependencies` edges
inverted, and a lock file that stores the same fact twice is a lock file that
can contradict itself.

A leading `version` key covers the lock format itself. An unknown lock version
is discarded and the graph re-resolved, following the same fail-safe rule the
WSDB uses: never trust a file you cannot fully read.

## Determinism

Exact versions make most of this free. There is no selection step to be
non-deterministic about: the graph is fully determined by the manifests, and
resolution walks it rather than searching it. Given the same manifests, with or
without a lock file, the result is the same graph.

Two places still need care:

- Packages are visited in a defined order, not in manifest-declaration order and
  not in the order network responses arrive, so that the lock file's contents do
  not depend on which request finished first.
- Failure is deterministic too: the same conflicting graph reports the same
  conflict, not whichever one happened to be noticed first. Where the conflict
  search of the previous section proposes a version, it enumerates candidates in
  descending semver order, so the proposal is a function of the graph and not of
  the search's timing.

The lock file is therefore not what makes resolution reproducible — the exact
versions already do. What it adds is the transitive graph and the checksums,
which is reproducibility of the *bytes* rather than of the version numbers.

`molto build` uses the lock file when it is present and consistent with the
manifest, and re-resolves only what the manifest changed.

`molto update` is the one command that goes looking for newer releases. Because
the manifest names exact versions, updating cannot mean "re-resolve within the
constraints" — there are none. It means: ask the registry what is newer, show
what would change, and **rewrite `Project.toml`** with the new numbers. That is
the only write to the manifest Molto ever performs other than `add` and
`remove`, it happens because the user asked for it explicitly (RFC-0003 permits
exactly that), and it leaves the upgrade in the diff where it can be reviewed.

## What is cached and what is not

The rule is inherited from `spec.md` §9 and is worth restating because it is
counter-intuitive: **source repositories are never cached; reusable build
artifacts are.**

A cloned repository is large, is only useful at one exact revision, and is
something the user could have cloned themselves. What is worth keeping is what
was built from it, keyed by everything that went into the build — the source
identity, the flags, the profile, the toolchain — in the content-addressed
global cache described in RFC-0007. Two projects that depend on the same
version of the same library, built the same way, should compile it once.

Path dependencies are excluded from this entirely. Their bytes change without
notice, and a cache entry for a moving target is a stale entry waiting to
happen.

## Implementation Status

None of this is implemented. `[deps]` and `[registries]` are not read, there is
no dependency type, no semver comparator, no lock file, no fetcher for any of
the five sources, and no global cache. `molto add`, `molto remove` and
`molto update` exit with code 5 as RFC-0002 requires, which is the correct
behaviour for an unimplemented command and not a placeholder to be replaced by a
partial one.

Molto also has no progress reporting of any kind, which the conflict search
needs. pickup already has it — a four-frame spinner drawn on stderr only when
stderr is a terminal, and wiped before the real output is printed — and it is a
self-contained utility to port rather than to reinvent.

The order the work has to happen in is fixed by dependencies between the pieces:
inline tables in the parser, then a reader for the two dependency tables, then
the semver comparator, then resolution against a single source (`path` is the
one with no network), then the lock file, then the registry client, then the
conflict search and its prompt, then the global cache.

## Non-Goals

Molto does not resolve system libraries. `[target].link` names a `-l` and the
system linker finds it or does not; there is no attempt to model, version or
install what the platform already provides.

Molto does not build a dependency in a way its own manifest does not describe.
A registry dependency is a Molto package built like any other; anything with a
bespoke build is a recipe (RFC-0009), and the recipe names an existing build
system rather than a script.

## Reserved / Future

- `[features]`, `optional` and `default_features`. RFC-0003 reserves all three
  and no algorithm is specified here, because feature unification is a second
  resolution pass over a graph that does not exist yet — and because a feature
  no manifest can enable does not need an algorithm.
- `[build-deps]`. Dependencies needed to *run* a build rather than to link into
  it. They wait because Molto has no build scripts, so nothing could consume one
  yet. `[dev-deps]` is no longer reserved; it is specified above.
- Vendoring — a command that copies the resolved graph into the project for
  offline or audited builds.
- Multi-package workspaces, where several members share one resolution and one
  lock file. Reserved by RFC-0004 for the same reason.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md) — `add`, `remove`, `update`, and exit code 3 for a failed resolution
- [RFC-0003: Project Manifest](0003-project-manifest.md) — the `[deps]` and `[registries]` tables this RFC consumes
- [RFC-0004: Workspace Specification](0004-workspace-specification.md) — why the resolution is not stored in the WSDB
- [RFC-0007: Build System](0007-build-system.md) — the build graph these edges extend, and the global cache
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — the `recipe` source
- [RFC-0010: Registry Specification](0010-registry-specification.md) — where a `version` source is resolved

See also `spec.md` sections 7 (Dependency Model), 9-10 (Artifacts and Global
Cache) and 15-16 (Registries).
