# Bill of materials

`molto metadata` answers one question: what is inside this binary, where did it
come from, and under what licence.

```sh
molto metadata                  # CycloneDX 1.6 to stdout
molto metadata -o sbom.json     # to a file
molto metadata --include-dev    # also what only [dev-deps] reaches
```

The output is [CycloneDX 1.6](https://cyclonedx.org) JSON, which
Dependency-Track, Grype and Trivy read without translation.

## What it contains

| Section | What is in it |
|---|---|
| `metadata.component` | your package: name, version, and whatever `[package]` declares — description, licence, authors, homepage, repository |
| `components[]` | every package the build links, one entry each |
| `dependencies[]` | the edges, including your own package's |

Each component carries its exact version, the licence its recipe declared, the
SHA-256 of the bytes that were fetched, and a `molto:source` property holding
the origin with its revision already resolved — `registry+https://…`,
`git+https://…#5a1e8ff…`, `path+modules/http`.

## Three things worth knowing

**A `path` dependency has no checksum, and no version.** Its bytes are whatever
is on disk, which is the point of one. It appears in the document with
`molto:unverified = true`, and `molto metadata` says so on stderr. Nothing is
silently omitted: a bill of materials that hides what it could not verify is
worse than none.

**Development dependencies are out by default.** A bill of materials describes
what ships. `--include-dev` adds them with `"scope": "excluded"` — CycloneDX for
"in the graph, not in the artifact".

**The document has no timestamp.** Two runs over the same graph produce the same
bytes, so it can be committed, diffed and compared between machines. That is
deliberate, and it is why there is no `serialNumber` either. Both fields are
optional in the schema, so what comes out is still a conforming 1.6 document.

## Where the licences come from

From `[package]` for your own package, and from each dependency's `recipe.toml`
`[about]` table for the rest. A dependency whose recipe states no licence gets
no `licenses` field — an empty string would be a claim, and silence is not one.

If a component in your report has no licence, the fix is upstream: the recipe
that publishes it needs an `[about]` table. See `docs/Project.md` for the keys.

## What it costs

Resolving the graph, which is what reads the recipes. In a project that has
already been built this touches no network — a published coordinate never
changes, so the registry's previous answer still stands, and the sources are
already in `~/.molto/cache`. In a fresh clone it fetches, exactly as a build
would.

It does not read `Molto.lock`. The lock records versions, origins and checksums
and deliberately not licences: that fact already lives in each recipe, and a
lock file that stored it twice could contradict itself.

## Related

- [`Project.md`](Project.md) — the `[package]` keys this reports
- [`rfcs/0002-cli-specification.md`](../rfcs/0002-cli-specification.md) — the command
- [`rfcs/0009-recipe-specification.md`](../rfcs/0009-recipe-specification.md) — `[about]` in a recipe
