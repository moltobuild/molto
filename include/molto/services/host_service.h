#ifndef MOLTO_HOST_SERVICE_H
#define MOLTO_HOST_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <molto/project/project_ctx.h>

/*
 * Libraries the host already provides (RFC-0016).
 *
 * A manifest names a capability — `gtk+-3.0` — and this asks a resolver where
 * it is. On Linux and macOS that resolver is `pkg-config`, whose module names
 * are the one thing about a system library that is stable across distributions:
 * the Debian package is `libgtk-3-dev`, the Arch one is `gtk3`, the Fedora one
 * is `gtk3-devel`, and all three answer to `gtk+-3.0`.
 *
 * What comes back is bounded on purpose to include directories and link flags.
 * A resolver that could contribute a `-D` would be deciding a consumer's ABI
 * from outside its manifest, and one that could contribute anything at all
 * would be a second, unreviewed source of compile options — which is what
 * RFC-0013's refusals exist to prevent. `pkg-config` does emit other things;
 * they are dropped, and dropping them is the contract rather than an omission.
 *
 * This is not `[deps]` and shares nothing with it. Molto does not fetch, build,
 * version or install what it finds here; it asks where the headers are and
 * stops. A capability that is absent is a build that fails with a message
 * naming what to install, which is the whole improvement over a compiler
 * reporting a missing file.
 */

/* What answered, for the lock to record. A name and not a path: which
   pkg-config was run is this machine's business, and a lock that carried it
   would differ between two machines that agreed about everything that matters. */
#define HOST_RESOLVER_NAME "pkg-config"

/* Sized from what is on a shelf rather than from a round number. `gtk+-3.0`
   answers with 25 include directories on an ordinary Debian, `gtk+-unix-print-3.0`
   with 26 and `webkit2gtk-4.0` with 29 — the toolkits are the reason this key
   exists, so a limit that refuses them would be a limit that refuses the case.
   Overflowing is an error and never a truncation: a dropped `-I` is a header
   not found, reported by the compiler, naming neither the capability nor the
   resolver that answered. */
#define HOST_MAX_INCLUDES 64
#define HOST_MAX_LINKS 64
#define HOST_PATH_MAX 512
#define HOST_FLAG_MAX 128

/* What one capability contributes to a build. */
typedef struct {
    char includes[HOST_MAX_INCLUDES][HOST_PATH_MAX];
    size_t include_count;
    /* As they reach the link line — `-lgtk-3`, never `gtk-3` — which is the
       rule a `LinkOption` already follows (RFC-0013 schema 3). */
    char links[HOST_MAX_LINKS][HOST_FLAG_MAX];
    size_t link_count;
    /* What the resolver said it found, for the lock to record. Empty when the
       resolver knows of no version. */
    char version[64];
} host_answer;

/* Ask the resolver about one capability.
 *
 * False with a message in `err` when no resolver is installed, when the
 * capability is unknown to it, or when the answer does not fit. The message
 * names the capability and says what would answer it, because "install
 * libgtk-3-dev" is actionable and "fatal error: gtk/gtk.h: No such file" is
 * not. */
[[nodiscard]] bool host_resolve(const char *capability, host_answer *out, char *err,
                                size_t err_size);

/* Every capability a target declares, resolved in order.
 *
 * `out` is caller-provided with room for `target->host_count` answers. The
 * first failure stops the walk and is reported: a build missing one of its
 * libraries is not a build, and reporting the rest would bury the first. */
[[nodiscard]] bool host_resolve_all(const project_target *target, host_answer *out, char *err,
                                    size_t err_size);

#endif /* MOLTO_HOST_SERVICE_H */
