# RFC 0010: Registry Specification

- RFC Number: 0010
- Title: Registry Specification
- Status: Draft
- Created: 2026-08-07

## Summary

This RFC specifies the **protocol** between Molto and a Registry: the HTTP
surface for reading a catalogue and downloading an artifact, the two-step
publication flow, the immutability and integrity rules a conforming registry
**MUST** enforce, how a client authenticates, and how a private registry is
selected and reached.

It is a protocol, not an implementation. `spec.md` §16 promises the protocol is
public and the backend is free; this document is what makes that promise
checkable. It formalises `spec.md` sections 15 and 16, and completes RFC-0009
by saying how a recipe is published and served.

## Motivation

A registry exists and is deployed, `molto login` and `molto publish` reach it,
and none of it is written down. The consequences are already visible.

`molto login` is implemented in the binary and appears in no RFC — RFC-0002
lists `publish` and `update` and does not mention it. A command that stores a
credential on a user's disk is the last one that should be undocumented.

The client stores exactly one registry URL, while RFC-0003 specifies
`[registries]` as a map of names to URLs and lets a dependency select one. Those
two designs cannot both be right, and nothing currently forces the question.

And the division of labour between client and registry is a set of decisions
nobody can see: that the registry never resolves a version, that it measures a
checksum rather than believing one, that bytes are uploaded before the catalogue
row is written. Each of those is deliberate, each is load-bearing, and a second
implementation would get all three wrong by default.

## Shape of the protocol

Every endpoint is under `/v1`. The prefix is the protocol version, and a
breaking change to the surface is `/v2` rather than a negotiated header — an
ecosystem where the client and the registry are upgraded independently should
not have to negotiate anything at connection time.

Requests and responses are JSON, with two exceptions: an artifact's bytes are
uploaded as `application/octet-stream` and downloaded as a stream, and a recipe
is uploaded as `application/toml`, because the registry stores the document the
publisher wrote and a client that re-encoded it into JSON would be publishing
its own parse rather than the author's file.

Errors are always the same object, regardless of endpoint:

```json
{ "error": "conflict", "message": "yyjson 0.10.0 (any) is already published" }
```

`error` is a stable machine-readable code; `message` is for a person and may
change. The mapping is fixed:

| Code | HTTP | Meaning |
|---|---|---|
| `invalid_request` | 400 | The request did not match the schema |
| `invalid_argument` | 400 | The request was well-formed and wrong |
| `unauthorized` | 401 | Missing or unusable credential |
| `not_found` | 404 | No such coordinate |
| `conflict` | 409 | The coordinate is already published |
| `not_implemented` | 501 | The registry does not offer this |
| `internal_error` | 500 | Anything else |

A client distinguishes a transport failure from an HTTP status. A 404 is an
answer — the registry was reached and said no — and only an unreachable
registry, an unparseable body or a missing status is a failure of the call
itself.

## Reading

`{kind}` in every path below is the plural of a recipe kind (RFC-0009):
`toolchains`, `packages` or `tools`. The same routes are registered once per
kind, because the three differ in what they contain and not in how they are
addressed.

| Method | Path | Answers |
|---|---|---|
| GET | `/v1/health` | that the registry is up |
| GET | `/v1/search?q=&kind=&limit=` | artifacts matching a query |
| GET | `/v1/{kind}` | the names published under a kind |
| GET | `/v1/{kind}/{name}` | the versions of a name |
| GET | `/v1/{kind}/{name}/{version}` | the targets of a version |
| GET | `/v1/{kind}/{name}/{version}/{target}` | one artifact's metadata |
| GET | `/v1/{kind}/{name}/{version}/{target}/download` | the bytes |

Reads are unauthenticated on a public registry. An artifact's metadata carries
the whole parsed recipe, so a client can read a dependency's contract — what it
links, what it includes, what it needs — without downloading the archive first.
That is what makes resolution possible over a slow connection.

A download response carries `x-molto-checksum: sha256:<hex>`, an `etag`, and
`cache-control: public, max-age=31536000, immutable`. The immutable cache
directive is not an optimisation; it is the coordinate immutability rule
expressed to every HTTP cache between the two ends. It is also why the client
**MUST** verify the checksum after downloading: a response that can be cached
forever anywhere is a response whose integrity cannot rest on the transport.

### The registry does not resolve versions

`GET /v1/{kind}/{name}` returns version strings. It does not accept `^1.2`, it
does not know what "latest" means, and it applies no ordering beyond what it
was given.

Most of the time there is nothing to resolve: a manifest names an exact version
(RFC-0008) and the client asks for that coordinate directly. Ordering is needed
only where Molto proposes a version rather than obeying one — `molto add`
without a version, `molto update`, and the conflict search — and in all three
the ordering happens on the client.

Keeping it there is deliberate. Semver is a dialect: pre-release ordering and
build-metadata rules are conventions, and baking them into the protocol would
force every private registry — a Postgres box, a static file server, a Worker —
to reimplement Molto's comparator identically or propose different versions for
the same graph. Serving opaque strings makes a conforming registry something
anyone can write in an afternoon, and keeps the answer identical everywhere
because there is only one implementation of it.

What the registry **does** have to serve for the conflict search to work is
recipes without archives: `GET /v1/{kind}/{name}/{version}/{target}` returns the
whole parsed recipe, including its `[deps]`, so a client can walk a dependency
graph over metadata alone and never download a package it will not use.

## Publishing

Publication is two requests, and the order is part of the protocol:

1. `PUT /v1/{kind}/{name}/{version}/{target}/blob` — the archive, as
   `application/octet-stream`, with the declared digest in `x-molto-checksum`.
   Answers 201.
2. `POST /v1/{kind}` — the `recipe.toml`, verbatim, as `application/toml`.
   Answers 201.

**The bytes first and the row last.** If publication is interrupted between the
two, what remains is a blob nobody references: invisible, harmless, and
reclaimable by a sweep. The reverse order leaves a catalogue entry pointing at
nothing, which is visible to every client, breaks every resolution that touches
it, and cannot be repaired by anyone but the publisher. Both orders can fail;
only one fails safely.

Three invariants a conforming registry **MUST** enforce:

- **Size and checksum are measured, never accepted.** The recipe carries no
  `sha256` or `size` key by design. The registry hashes the bytes as they land
  and records what it measured. A registry that trusted the publisher's number
  would be a registry whose integrity guarantee restates the publisher's claim.
- **The coordinate is checked before the upload and again before the insert.**
  An already-published coordinate is a 409 at step 1, so a large upload is
  refused before it is spent rather than after.
- **The recipe is validated by the registry itself.** `molto publish` validates
  it too, and that is worth doing for the error message, but the registry cannot
  take a client's word for the coordinate it is about to make permanent. In
  particular, the `kind` in the recipe must match the endpoint it arrived at.

## Immutability, and yank

A published coordinate is never overwritten and never deleted. There is no
`PUT` that replaces an artifact and no `DELETE` that removes one. A change is a
new version.

The reason is that anything else breaks builds that already work. Someone's lock
file (RFC-0008) pins a version and a checksum; if the bytes behind that
coordinate can change, the lock file guarantees nothing, and if they can vanish,
every build that pinned them fails at once with no recourse.

What a publisher needs instead is **yank**: marking a version as one that new
resolutions must not choose, while every existing lock file that already names
it keeps working. A yanked artifact is still downloadable and is flagged with
`x-molto-yanked` so a client can warn.

Two endpoints write the flag, and they carry the same authorisation publishing
does — withdrawing someone's version is as consequential as adding one:

```
POST /v1/{kind}/{name}/{version}/{target}/yank     204
POST /v1/{kind}/{name}/{version}/{target}/unyank   204
```

They are separate paths rather than one path with a body, so that no client
withdraws a version by getting a boolean the wrong way round. Both are
idempotent: a retry after a lost answer is not a failure. A coordinate that was
never published answers 404, which is the only case worth reporting.

A yank writes one column. The storage key, the checksum, the size and the
metadata are exactly what they were, because those are what a lock file pinned.

## Authentication

`POST /v1/auth/token` exchanges an email and a password for a token, answering
201 with the token in the body. It is shown once and stored by the client; the
registry keeps only a hash of it, so a compromised catalogue database does not
yield usable credentials.

Every write carries `Authorization: Bearer <token>`. Reads carry nothing on a
public registry.

A token has a **kind**, and this is a conformance rule rather than an
implementation detail:

> A session credential **MUST NOT** authorise publication. Only a token issued
> for a command-line client may write.

A registry with a web interface has session cookies, and a cookie is sent by the
browser on any request the browser is convinced to make. Publishing an artifact
under someone's name because they visited a page is a supply-chain attack with a
trivial payload, and the separation costs one column. The browser authenticates
a person to a website; a token authenticates a client to an API.

Passwords are stored with a memory- or iteration-hard KDF and a per-user salt,
never with a plain digest.

## `molto login`

RFC-0002 does not list `molto login`, and the command exists. Specified here:

`molto login` prompts for an email and a password, with echo disabled, and
refuses to prompt when standard input is not a terminal — a password typed into
a pipe ends up in a shell history or a CI log. `--token` stores a token directly
and is the form CI is expected to use. `--registry` selects the URL.

The credential is written to `~/.molto/credentials.toml`, created with
owner-only permissions from the start rather than relaxed afterwards, and never
passed on a command line where another user's `ps` would see it.

The token is a credential the user can revoke from the registry, and the file
says so in a comment, because the recovery path for a leaked token should be
readable from the file that holds it.

## Private registries

The protocol above is the whole contract. Any backend that serves it is a
registry: `spec.md` §16 names Postgres, MySQL, SQLite and D1 for the catalogue
and R2, S3, Azure Blob and MinIO for the bytes, and the protocol mentions none
of them.

A private registry is declared in `[registries]` (RFC-0003) and selected per
dependency with `registry = "<name>"`. A private registry may require
authentication on reads as well as writes; a client that receives 401 on a read
reports it as "not logged in to this registry", not as a missing package.

**A mismatch to resolve.** `credentials.toml` holds a single `[registry]` table
with one URL and one token. Named registries have nowhere to keep a credential,
so a project that depends on packages from two registries cannot authenticate to
both. The correct shape is one table per registry URL:

```toml
[registry."https://packages.myorg.dev"]
email = "…"
token = "…"
```

This is specified and not implemented. Nothing publishes to two registries yet,
which is why the shortcut was taken and why it is cheap to fix now rather than
after credentials exist in the field.

## Implementation Status

Implemented and conforming: the `/v1` prefix and error object, health, search,
all five catalogue reads, download with its checksum and cache headers, the
two-step publish with measured size and checksum, coordinate conflict detection
before both steps, registry-side recipe validation, bearer tokens restricted to
the client kind, and hashed token storage.

Not implemented:

- **Yank.** The flag is stored and served; no endpoint sets it.
- **Per-registry credentials**, as above.
- **Pagination** on the catalogue listings. Every name and every version is
  returned in one response, which is correct today and will not be.
- **Ownership.** Any authenticated account may publish any name. There is no
  owner, no team, no name reservation and therefore no defence against
  squatting or against a typo publishing under someone else's name. This is the
  most serious gap in this document.
- **Token management over the API.** Tokens can be created and, through the web
  interface, revoked; there is no way to list or revoke them from the CLI, and
  they do not expire.
- **Rate limiting** and download counters.
- A **metadata-only** endpoint for a recipe. The recipe is reachable inside an
  artifact's metadata, which works and is not the obvious place to look.

## Non-Goals

A registry does not build anything. It stores bytes and the document describing
them; producing those bytes is the publisher's job, and consuming them is
Molto's.

A registry does not resolve dependencies. It serves one artifact's declared
dependencies as part of its recipe and never walks the graph — that is
RFC-0008's work, on the client, where the lock file is.

A registry is not a mirror of anything. There is no proxying of other
registries, no fallback chain, and no implicit upstream: a dependency names the
registry it comes from, or it comes from the official one.

## Reserved / Future

- **Signing.** Checksums prove the bytes did not change in transit; a signature
  would prove who produced them. It matters more once ownership exists.
- **Mirrors and offline use**, including a way to point a whole workspace at a
  replica without editing every dependency.
- **Deprecation**, a softer signal than yank: still resolvable, but reported.
- **Statistics** — downloads, dependents — which are the reason to add
  pagination properly rather than as an afterthought.

## Related RFCs

- [RFC-0001: Manifesto](0001-manifesto.md)
- [RFC-0002: CLI Specification](0002-cli-specification.md) — `publish`, `update` and `login`
- [RFC-0003: Project Manifest](0003-project-manifest.md) — `[registries]`, and the per-dependency `registry` key
- [RFC-0008: Dependency Resolution](0008-dependency-resolution.md) — the client half of version selection, and the lock file this protocol's immutability protects
- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — the document published here

See also `spec.md` sections 15 (Registry Philosophy) and 16 (Private
Registries).
