# RFC 0016: Host Libraries

- RFC Number: 0016
- Title: Host Libraries
- Status: Draft
- Created: 2026-08-27

## Summary

This RFC specifies how a project or a recipe names a library **the host already
provides**, so that its include directories reach a compile line without any
manifest spelling out one machine's filesystem layout.

It narrows a Non-Goal. RFC-0008 says Molto "does not resolve system libraries",
and that stays true of *installing*, *versioning* and *modelling* them. What
this adds is the one thing that sentence made impossible by accident: asking
where the headers are.

## Motivation

`[target].link = ["z"]` works today. The linker finds `libz` or it does not, and
when it does not the message is the linker's own and it is precise:

```
cannot find -lnosuchlib: No such file or directory
```

That covers every library whose headers live in the compiler's default path,
which is why it has never looked like a gap. `#include <zlib.h>` and
`#include <openssl/ssl.h>` both compile with nothing declared.

The gap is the other half of the shelf. Measured with `pkg-config` on one
ordinary Linux workstation, **35 of 60 packages emit non-empty cflags** — an
include directory the compiler does not know about:

```
glib-2.0   -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include
libpng     -I/usr/include/libpng16
cairo      -I/usr/include/cairo -I/usr/include/glib-2.0 …
```

Two of those are worth staring at. `libpng16` carries a version in the path, so
it changes under you. `glib-2.0`'s second path is **architecture-specific**, so
it differs between two machines running the same distribution.

And a manifest cannot name them anyway. Since the build lowers everything
through the document (RFC-0013), a target's include paths are checked against
the workspace, the build directory, the cache and the resolved dependencies:

```
molto: an include path of target 'app' resolves to '/usr/include/glib-2.0',
       which is outside the workspace, the build directory, the cache and every
       dependency this build resolved
```

That check is right and was added for a real reason: a dependency's recipe,
written by a remote party, was putting `-I` anywhere it liked. But it applies to
the user's own manifest too, which is a rule the asymmetry argument of RFC-0014
says should be looser there — and the effect is that **a project needing glib
cannot be built by Molto at all.** Not awkwardly. Refused.

So the choice is not "should Molto grow pkg-config". It is: a library on the
host is either nameable or it is not, and today more than half of them are not.

## What this is not for

This RFC is deliberately small, and the boundary is drawn from a measurement
rather than from taste. SDL3's CMake configuration asks the host **153**
questions, of which 56 are `check_c_source_compiles` — a distinct C program per
question, compiled to find out whether one macro or struct member exists in one
of *your* system headers. Worse, the answers decide what gets built: of 238
calls that add sources, **185 are inside a conditional**, and 83 driver defines
are set by the same logic.

A source list that is a function of the host is not a description, and this
format describes. Nothing here brings SDL3, GTK or anything shaped like them
closer, and no extension of this table should be proposed for them: that road
ends at running the dependency's own build system, which is RFC-0009's
`via = "delegate"` and a different decision entirely.

**The case this serves** is a library that is present or absent and nothing
else: it is there, here is where its headers are, link it. glib, libpng from the
distribution, ncurses, OpenSSL where its headers are not in the default path.
One question, one answer, no probes.

## The shape: a capability, never a path

A manifest names **what it needs**, and something else answers **where it is**:

```toml
[target]
host = ["gtk+-3.0"]
```

A key of its own and not an extension of `requires`, which an earlier revision
of this section proposed. Implementing it settled the question: `requires` is
already serialised into a request for **pickup**, about a compiler, and one list
answered by two resolvers is a list nobody can read — a `gtk+-3.0` in it would
be asked of the wrong one. Two questions, two keys.

Never this:

```toml
include = ["/usr/include/glib-2.0"]   # this machine, this architecture, today
```

The reason is portability and it is the whole design. A manifest that spells a
path has already decided which distribution, which architecture and which
version of the library its reader has. A manifest that names a capability has
decided nothing: on Linux something answers with pkg-config's cflags, on Windows
with whatever that platform's convention is, on macOS possibly with a framework.
The manifest is the same file on all three.

This is not a new mechanism. `[target].requires` already names capabilities the
code needs — `c23`, `attr_nodiscard` — and **pickup** already answers them,
through a request string that is both the question and the fingerprint its
answer is cached under (`toolchain_service`). A host library is the same kind
of question asked of the same resolver. What this RFC adds is vocabulary, not
machinery, and a second key rather than a second meaning for an existing one.

## Rules

- **A capability is a name, not a path.** A value containing `/` is a rejected
  manifest, because it is the mistake this exists to prevent and it will
  otherwise be made by everyone whose machine happens to work.
- **The answer is not the manifest's to state.** Nothing in `Project.toml` says
  where a capability was found. That is the resolver's answer, and it lives
  where the toolchain answer lives.
- **An unanswered capability fails the build before it compiles**, naming the
  capability and the resolver that could not answer it. A missing header found
  by the compiler is a message about a file; a missing capability is a message
  about a dependency, and the second one says what to install.
- **What the answer contributes is bounded to include directories, `-l`, `-L`
  and `-Wl,-rpath`.** Not defines, not arbitrary flags. The rpath is in that
  list and the rest of `-Wl,` is not, which testing settled rather than taste:
  SDL3 in a custom prefix linked and then could not start, because `-L` says
  where a library is for the link and `-rpath` says it for the run. They are one
  fact spelled twice, and a resolver whose job is to locate a library owes both. A resolver that could contribute
  `-D` would be deciding a consumer's ABI from outside its manifest, and a
  resolver that could contribute anything at all would be a second, unreviewed
  source of compile options — the thing RFC-0013's option refusals exist to
  prevent.
- **The bound moves, the fence does not.** An answered capability's directories
  become a bound for that build, exactly as a resolved dependency's root is one
  today. Nothing gains the ability to name a path Molto did not resolve.

## What the lock records

A host library is not reproducible the way a package is, and the lock should
say so rather than imply otherwise. It records **that a capability was required
and what answered it** — the resolver, and the version string the resolver
reported where there is one:

```toml
[[host]]
capability = "glib-2.0"
answered = "pkg-config"
version = "2.80.0"
```

This is a record, not a pin. Molto cannot install glib and will not try, so a
machine that answers `2.79.0` is not refused — but the difference is visible,
which is the whole value. A build that behaves differently on two machines
should have something a reader can diff.

The alternative — recording nothing — was considered and rejected. It makes the
lock claim more than it knows: a file whose purpose is "a build on another
machine contains the same bytes as this one" should not silently omit the inputs
that are not under its control.

## Cross-platform from the start

Windows has no pkg-config, no `/usr/include`, and a different answer for every
question here. That is a reason to design for it now rather than to defer it,
because the cost of getting this wrong is not a port — it is a manifest format
that has to change.

The design survives it because a manifest never names a mechanism. `host =
["gtk+-3.0"]` says nothing about pkg-config; it is a question, and each platform
brings its own answerer:

| Platform | What answers |
|---|---|
| Linux, BSD | `pkg-config`, and the compiler's default paths for what needs none |
| macOS | `pkg-config` under Homebrew or MacPorts; frameworks are a second shape |
| Windows | vcpkg's layout, or a per-capability path the user configured |

Two things follow, and both are consequences of the table rather than
decisions taken separately. A capability name is **not** guaranteed to be a
pkg-config module name — it is a name this ecosystem agrees on, and a Linux
answerer maps it. And a manifest that requires a capability no platform can
answer fails **on that platform only**, which is what per-target support in
RFC-0009's Reserved section is for.

## Implementation Status

Nothing. This RFC specifies and implements none of it.

Implemented in molto 0.32.0: `[target].host` and its refusal of a value carrying
a path separator (`project_ctx`), the resolver itself (`host_service`, asking
pkg-config and keeping only `-I`, `-l` and `-L` from its answer), the answer
reaching the document as `IncludePath` nodes marked `system` and `LinkOption`
nodes (`frontend_native`), and the answered directories joining the bounds a
document is validated against (`build_service`) — resolved a second time from
the manifest rather than read back off the document, so nothing a producer wrote
can widen what it is held to.

A GTK-3 application builds: `host = ["gtk+-3.0"]` alone, 25 include directories
and 12 link flags, on Debian's own GTK.

The `[[host]]` section arrived in 0.33.0, and with one rule this document did
not anticipate: **what the lock recorded is kept rather than replaced with this
machine's answer.** Rewriting it every build would make two developers on
different distributions flip the file back and forth in every commit, and the
diff that was supposed to make a difference visible would become the noise
nobody reads. A capability the lock has never seen is recorded now; one it has
is left alone, and the note is what says this machine disagrees.

What does not exist: any resolver that is not pkg-config, and a capability in a
recipe rather than a manifest.

## Non-Goals

Molto does not install a host library, and will not grow a flag that does. A
capability that is absent is reported; what to do about it belongs to the
machine's own package manager, and a build tool that started installing system
packages would be one.

Molto does not version-constrain a host library. `host = ["gtk+-3.0"]` is a
question about presence, and the lock records what answered. A range would be a
promise Molto cannot keep, since it neither installs nor chooses what is on the
host.

Molto does not run a probe. A capability is answered by asking a resolver, never
by compiling a program the dependency wrote to see whether it succeeds — that is
the mechanism SDL3 needs 56 times, and it is the one this RFC exists to stay
away from.

Molto does not make the source list depend on the answer. Every target's sources
are what the manifest or recipe says, whatever a capability answered. A project
that wants to build differently when a library is absent is describing two
builds, and this format describes one.

## Reserved / Future

- **Frameworks**, as a second shape of answer on macOS: `-framework Cocoa` is
  neither an include directory nor a `-l`.
- **A capability in a recipe**, so a source recipe can require one. Specified
  here for `[target]` only, because a manifest is the reviewed file and the
  looser case should be understood before a remote party can ask for it.
- **Per-target requirements**, which RFC-0009 reserves for the same reason: one
  recipe describing a build that differs on Windows without becoming three.

## Related RFCs

| Question | Read |
|---|---|
| Why versions are exact, and the system-library Non-Goal this narrows | `rfcs/0008-dependency-resolution.md` |
| `[target].requires` as it exists, and the manifest's shape | `rfcs/0003-project-manifest.md` |
| The bounds a path is checked against, and why a plugin's are stricter | `rfcs/0013-build-intermediate-representation.md` |
| `via = "delegate"`, the road this RFC declines to take | `rfcs/0009-recipe-specification.md` |
