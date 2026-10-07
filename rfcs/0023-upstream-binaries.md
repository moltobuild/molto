# RFC 0023: Upstream Binaries

- RFC Number: 0023
- Title: Upstream Binaries
- Status: Draft
- Created: 2026-10-07

## Summary

A binary recipe may name the archive its upstream publishes — a URL, its sha256,
its size and the directory inside it to keep — instead of an archive uploaded
to the registry. The registry stores the recipe and nothing else, and answers
for the artifact with the upstream URL; pickup downloads from there, verifies
the digest the recipe pinned, and installs what is under that directory.

The first two are CMake and Ninja, as `kind = "tool"` with the new
`[tool].kind = "build"`: what a dependency configured with CMake needs on the
machine that configures it (RFC-0009, `via = "delegate"`).

## Motivation

The registry hosts nothing third parties publish. A source recipe names the
upstream tarball (RFC-0009), a platform recipe the files each platform's own
repository publishes (RFC-0022). A binary recipe was the exception: its archive
was uploaded, so offering CMake through pickup meant copying Kitware's binaries
into this registry and serving them as ours.

Upstream already publishes what a binary recipe needs. Kitware releases CMake
for Linux (x86-64, aarch64), macOS (universal) and Windows with a SHA-256 list;
the Ninja project releases a zip per platform. A recipe that pins those bytes
by digest gives every machine the same tool, downloaded from where its authors
put it, and verified against what the recipe published.

## The recipe

```toml
schema = 1
form = "binary"
kind = "tool"
name = "cmake"
version = "4.4.4"
target = "linux-x86_64"

[archive]
url = "https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-linux-x86_64.tar.gz"
sha256 = "…"
size = 64865570
format = "tar.gz"
strip_prefix = "cmake-4.4.4-linux-x86_64"

[tool]
kind = "build"
binary = "bin/cmake"
```

| Key | Type | Description |
|---|---|---|
| `url` | string | Where upstream publishes the archive; `https` only |
| `sha256` | string | The digest of those bytes, lowercase hex |
| `size` | integer | Their size in bytes, which a download reports progress against |
| `format` | string | `tar.gz`, `tar.xz`, `tar.zst` or `zip` |
| `strip_prefix` | string | The directory inside the archive whose contents are the install; absent means the archive's top level |

One recipe per target, as every binary recipe is: what differs between
platforms is the URL and the digest, and a target is what a client asks for.

- **`url` is a promise about a location, `sha256` about content.** A client
  verifies the digest before unpacking a byte, so an upstream that re-rolls or
  moves a release breaks an install loudly instead of changing it.
- **`strip_prefix` is a path, not a count.** CMake's macOS build keeps its tree
  three directories down (`cmake-4.4.4-macos-universal/CMake.app/Contents`); a
  path says which directory, and a count would only say how deep.
- **The tree is kept whole.** CMake finds its modules relative to its binary
  (`share/cmake-4.4`), so an install is everything under `strip_prefix`, not
  the binary alone.
- **`[tool].binary` is a path inside the install**: `bin/cmake`, or `ninja` for
  an archive that holds nothing else.

## `[tool].kind = "build"`

A tool a build runs rather than a tool that reads code: CMake and Ninja next to
the formatter, the linter and the linker. `pickup install cmake` installs it,
and `pickup tools --format toml` — which molto already reads to find
`clang-format` — reports where it is. molto asks for one when a dependency it is about to configure
says `[build] system = "cmake"`; that delegation is RFC-0009's to specify and
comes after this one.

## The registry

A binary recipe with `[archive]` is published without an upload: the recipe is
the whole publication, as a source recipe's is.

- It is catalogued as `form = "binary"`, with `checksum`, `size_bytes` and
  `format` taken from `[archive]`, and no storage key.
- Its `download_url` is `[archive].url`. `GET …/{target}/download` redirects
  there and counts the download.
- `url` must be `https`, `sha256` a digest, `size` a positive integer,
  `format` one of the four, and `strip_prefix` a relative path without `..`.
- A binary recipe has `[archive]` or an uploaded blob, never both.

## pickup

pickup reads an artifact as it always has — `download_url`, `checksum`,
`size_bytes`, `format` — so the download and the verification do not change.
What changes is the unpacking:

- **`strip_prefix`**: only that subtree is extracted, with as many leading
  directories removed as it has components.
- **`zip` on Linux**: GNU tar cannot read one, so pickup uses `unzip` there;
  bsdtar on macOS and Windows reads it as it is.
- **`kind = "build"`** joins the tools `pickup tools` lists and installs.

An older pickup handed such an artifact downloads and verifies it, finds no
binary at the top of the archive and refuses the install — a failure, not a
wrong tool.

## Non-goals

- **Building tools from source.** A tool whose upstream publishes no binary for
  a target has no recipe for that target.
- **Mirroring.** If an upstream URL disappears, the recipe stops installing; a
  new version pins the new location. Keeping copies is what this RFC exists to
  avoid.

## Related RFCs

- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — binary recipes, `[tool]`, `via = "delegate"`
- [RFC-0010: Registry Specification](0010-registry-specification.md) — publication and download
- [RFC-0022: Platform Packages](0022-platform-packages.md) — the same principle for libraries every platform packages
