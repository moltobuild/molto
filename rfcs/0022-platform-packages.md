# RFC 0022: Platform Packages

- RFC Number: 0022
- Title: Platform Packages
- Status: Draft
- Created: 2026-10-05

## Summary

This RFC adds a third recipe form, `form = "platform"`, for a library that every
platform already packages and nobody should rebuild: GTK, and things shaped like
it. The recipe pins, per platform, the exact files that platform's own
repository publishes — a `.deb` from Ubuntu, an `.rpm` from Fedora, a
`.pkg.tar.zst` from Arch or MSYS2, a bottle from Homebrew — with a digest for
each, and says what a consumer compiles and links with once they are unpacked.

Molto downloads them with the same `curl` it fetches sources with, unpacks them
itself, and keeps them in its cache. No package manager runs: not `apt`, not
`dnf`, not `pacman`, not `brew`. The registry stores the recipe and nothing else.

## Motivation

`molto add gtk` cannot work today, and neither existing form can make it work.

A **source recipe** needs a build Molto can describe. RFC-0016 measured why GTK
is not one: its dependencies ask the host hundreds of questions, and the answers
decide what is compiled. Building GTK on the consumer's machine is RFC-0009's
`via = "delegate"`, which is a decision about running foreign code and not a
recipe.

A **binary recipe** needs bytes in the registry. GTK's closure on Windows is 84
packages and 128 MB; on macOS, 38 bottles. Re-hosting them means the registry
redistributes LGPL binaries it did not build, for every platform, forever, and
falls behind the moment upstream ships a fix.

And `[target].host` (RFC-0016) answers only where GTK is already installed with
its headers. On a stock Ubuntu desktop the runtime is there and `libgtk-4-dev`
is not; on Windows and macOS there is nothing at all.

Every one of those platforms already has a repository that publishes GTK, built
by people whose job is building it for that platform, signed and versioned. What
is missing is a way to name those files without running the tool that normally
installs them.

## The shape

```toml
schema = 3
form = "platform"
kind = "package"
name = "gtk"
version = "4.14.0"
target = "any"

[host]
pkgconfig = "gtk4"

[[platform]]
name = "ubuntu-24.04-x86_64"
os = "linux"
arch = "x86_64"
distro = "ubuntu"
distro_version = "24.04"
format = "deb"
runtime = "host"
upstream = "4.14.5+ds-0ubuntu0.10"
include = ["usr/include/gtk-4.0", "usr/include/glib-2.0"]
link = [":libgtk-4.so.1", ":libglib-2.0.so.0"]

[[platform.file]]
url = "https://snapshot.ubuntu.com/ubuntu/20261005T000000Z/pool/main/g/gtk4/libgtk-4-dev_4.14.5+ds-0ubuntu0.10_amd64.deb"
sha256 = "…"

[[platform]]
name = "macos-arm64"
os = "macos"
arch = "aarch64"
os_version_min = "15.0"
format = "bottle"
runtime = "bundled"
upstream = "4.24.1"
include = ["include/gtk-4.0"]
link = ["gtk-4"]
lib = ["lib"]

[[platform.file]]
url = "https://ghcr.io/v2/homebrew/core/gtk4/blobs/sha256:…"
sha256 = "…"
```

`schema = 3` because a reader that predates this form must refuse it rather than
read a recipe with no `[source]` as a binary whose upload failed.

### `[host]`

| Key | Type | Description |
|---|---|---|
| `pkgconfig` | string | The module RFC-0016's resolver is asked first |

Optional. When the host answers, nothing is downloaded and the answer is used
exactly as a `[target].host` capability's would be: its include directories and
its `-l`, `-L` and `-Wl,-rpath`. A Fedora workstation with `gtk4-devel`
installed builds against its own GTK, which is what its owner expects.

### `[[platform]]`

| Key | Type | Description |
|---|---|---|
| `name` | string | Unique within the recipe; the cache directory and every message use it |
| `os` | string | `linux`, `macos` or `windows` |
| `arch` | string | `x86_64` or `aarch64` |
| `distro` | string | Linux only: matched against `ID` in `/etc/os-release` |
| `distro_version` | string | Linux only: matched against `VERSION_ID`; absent for a rolling distribution |
| `os_version_min` | string | macOS only: the oldest release the files run on |
| `format` | string | `deb`, `rpm`, `pacman` or `bottle` |
| `runtime` | string | `host` or `bundled` |
| `upstream` | string | What the platform calls the version; informational |
| `include` | array[string] | `-I` directories, relative to the unpacked root |
| `link` | array[string] | Libraries, as `-l` takes them; `:soname` names one file |
| `lib` | array[string] | `-L` directories, relative to the root; also the rpath under `bundled` |
| `bin` | array[string] | Windows: directories whose DLLs are copied beside an executable |

Each `[[platform.file]]` carries `url` and `sha256`, both required. At most 256
files per platform and 16 platforms per recipe.

**The first platform that matches is chosen**, in recipe order, by `os` and
`arch` from how Molto itself was compiled and, on Linux, by `/etc/os-release`.
None matching is a refusal that lists the platforms the recipe does serve.

### `runtime`

The one decision a platform states rather than leaves to be inferred:

- **`host`** — the files are the development half only: headers, `.pc` files and
  the unversioned symlinks a `-dev` package ships. The libraries a program loads
  are the host's own, and `link` names them by soname (`:libgtk-4.so.1`) so the
  link reaches the runtime the host has rather than a symlink it may not. This
  is what Linux is for: GTK comes with the desktop, and a GTK the cache supplied
  would load its own Mesa next to the host's kernel driver. Before a build uses
  one, Molto checks every soname exists in the host's library directories and
  refuses, naming the package to install, when one does not.
- **`bundled`** — the files are the whole closure, runtime included. `lib` is
  added as `-L` and as `-Wl,-rpath`, so the program finds them without an
  environment variable, and on Windows every DLL under `bin` is copied beside
  the executable, which is where Windows looks.

## What Molto does with the files

Each file is fetched to `<cache>/downloads/<sha256>`, verified, and unpacked into
`<cache>/platforms/<name>/<version>/<platform>`, which carries the same stamp a
completed source fetch does. Molto unpacks every format itself or with `tar`,
which it already requires:

| Format | What it is | How |
|---|---|---|
| `deb` | an `ar` archive holding `data.tar.*` | the `ar` index is read in C; the member goes to `tar` |
| `rpm` | a lead, two headers and a compressed `cpio` | the headers are skipped and the `cpio` read in C |
| `pacman` | `tar` with zstd or xz | `tar` |
| `bottle` | `tar.gz` of `<formula>/<version>/…` | `tar --strip-components=2`, merging every keg into one prefix |

**zstd is decompressed by Molto, not by `tar`.** Ubuntu's data members,
Fedora's payloads and every Arch and MSYS2 package are zstd, and whether a
`tar` reads zstd is a property of how it was built: macOS's and Windows'
bsdtar hand it to a `zstd` program, which a stock Mac does not have — measured,
every Linux and Windows platform failed to unpack on one until this changed. So
Molto runs `zstd -d` itself and hands `tar` a plain archive. Every Linux this
form serves has `zstd` in its base install, because dpkg, rpm and pacman need
it. On Windows it is downloaded once, from the zstd project's own release,
pinned by digest like every other file, into `<cache>/tools`, and shared by
every project on the machine. `$MOLTO_ZSTD` names another.

A bottle is downloaded from ghcr.io with the anonymous token every public
Homebrew bottle accepts, which is the only header this form ever sends.

**A bottle is relocated, and then signed again.** Homebrew writes
`@@HOMEBREW_PREFIX@@` and `@@HOMEBREW_CELLAR@@` into every install name and
lets `brew` rewrite them on install. Molto rewrites them to `@rpath/<file>`
instead — shorter, so it fits in place — and rewrites nothing else. A Mach-O
whose load commands changed no longer matches its signature, and Apple silicon
refuses to load one, so each changed file is signed ad hoc with
`/usr/bin/codesign`, which is part of macOS.

Nothing in any package runs. A `.deb`'s maintainer scripts, an `.rpm`'s
scriptlets and a pacman `.INSTALL` are never read.

## Rules

- **Every file has a digest.** As with `[source]`'s archive, a URL is a promise
  about a location, and a recipe whose output changes when a mirror does is not
  a recipe.
- **Every path stays inside the root.** `include`, `lib` and `bin` are relative,
  hold no `..` and are refused otherwise; an archive member that would land
  outside the root is refused while unpacking, not after.
- **No `[source]`, `[build]`, `[[provide]]` or `[artifacts]`.** A platform recipe
  is nothing but this; declaring those beside it is two answers to one question.
- **No `[deps]`.** The closure is the platform's, pinned file by file. A
  dependency on a Molto package from a platform package would mix two worlds
  that version independently.
- **The version is an API floor.** Each platform ships its own GTK; the
  coordinate's version is the oldest of them, so a consumer that writes against
  it builds on every platform the recipe serves. Changing any pin is a new
  version (RFC-0010), and `upstream` is what records which build each platform
  got.

## What the lock records

The package, as for any other: its coordinate and, as its checksum, one digest
over **every** platform's pins. Which platform was chosen is not recorded, and
neither is a digest of only that platform's files — two developers on two
distributions resolve the same coordinate to two different platforms, and a
lock that recorded either would flip between them in every commit, which
RFC-0016's `[[host]]` already learned. Over every pin it is the same on every
machine, and it changes the moment any pin does: a recipe regenerated under a
version already in a lock is refused as the same coordinate serving different
bytes, which is what it is.

## Alternatives considered

- **Hosting the closure in the registry.** Rejected in the Motivation: it makes
  the registry a redistributor of binaries it did not build.
- **Resolving the closure on the consumer's machine** from `Packages.xz`,
  `repodata` and pacman databases. That is a package manager in C, and it makes
  `molto add` produce a different closure on Tuesday than on Monday. The recipe
  is generated by a script instead, once, and reviewed as a diff.
- **Running the platform's package manager.** It needs root, installs
  system-wide and differs per distribution — the three things RFC-0016's
  Non-Goals refuse.
- **Downloading the full closure on Linux too.** Measured: 196 Arch packages and
  309 Ubuntu ones, Mesa and LLVM among them. A program that loads a cache's Mesa
  instead of the host's breaks on the GPU the host actually has.

## Implementation Status

Molto's next release reads the form, chooses a platform, fetches, verifies and
unpacks all four formats, relocates and re-signs bottles, and folds the result
into a build: the includes as system ones, the link flags onto the link line
only, the host's directories into the document's bounds (RFC-0013), and on
Windows the DLLs beside each executable. `molto publish` sends one with no
archive. The registry validates it as strictly as Molto reads it, and stores it
with no blob, like a source recipe.

The first recipe is `gtk 4.14.0`, generated by `gtk/generate.py` in the recipes
repository: 271 files for Ubuntu 24.04 and 26.04, Fedora 44, Arch, MSYS2
mingw64 and macOS on Apple silicon. A GTK window opens on macOS from a project
whose manifest says `gtk = "4.14.0"` and nothing else; every other platform's
files were fetched, verified and unpacked with every include directory present.

A recipe this size is about 80 KB of JSON per release, and a package's listing
carries every release (RFC-0010), so a registry answer may now be a megabyte
rather than 64 KB. A listing that outgrows that is a registry that should page
it.

While a recipe is written it is tried as a path dependency,
`gtk = { path = "…/recipes/gtk" }`: a carried `recipe.toml` may be a platform
recipe too.

Not yet: data files a running GTK looks for at runtime on a bundled platform —
GSettings schemas, the gdk-pixbuf loader cache, icon themes — which Homebrew and
MSYS2 locate through their own prefix. A window opens without them; an image
loader or a file chooser may not.

## Related RFCs

- [RFC-0009: Recipe Specification](0009-recipe-specification.md) — the other two forms
- [RFC-0010: Registry Specification](0010-registry-specification.md) — a recipe with no blob
- [RFC-0016: Host Libraries](0016-host-libraries.md) — what `[host]` asks first
- [RFC-0018: The macOS Port](0018-the-macos-port.md) — install names and rpaths
