# Molto packages as dependencies

A library fetched through `git`, `archive` or `path` can describe itself with
`Project.toml`. It must declare its name, version and `artifact = "static"` or
`"shared"`, and contain sources under `src/`. The dependency key must equal the
package name. Carry exactly one description: a manifest and `recipe.toml`
together are an error, including in older tags.

```toml
[package]
name = "greet"
version = "1.0.0"
artifact = "static"
license = "MIT"
repository = "https://example.com/greet"
files = ["third_party/**", "data/table.inc"]

[target]
std = "c17"
include = ["include", "private"]
defines = ["PRIVATE_ONLY"]

[interface]
defines = ["PUBLIC_ABI"]
link = ["m"]
# include = ["generated"]
# flags = ["--coverage"]
# entry = "src/framework_main.c"
```

Consumers receive `include/` automatically. Do not repeat it under
`[interface].include`; other public header directories go there. Target include
paths, defines and flags stay private while compiling a dependency. Its
language standards and compiler requirements still apply, and both link lists
reach the final executable. Its development dependencies, test settings,
profiles, environment and plugins are ignored.

Only sources under `src/` compile for consumers. `src/main.c` is excluded.
An optional `[interface].entry` compiles into `<name>.entry.a`, after the
ordinary objects on the link line: it supplies `main()` when the consumer has
none. Keep registration constructors and other required functionality in
ordinary sources. Two entries in one binary are refused.

Git and archive manifest packages retain their manifest, sources, public and
private header trees, entry, root `LICENSE*`, `NOTICE*`, `COPYING*` files and
`[package].files` matches. Pruning happens before installation; its version is
recorded in the completion stamp. Path dependencies and recipe-directed
sources are never pruned.

Before tagging a package, run:

```sh
molto package --list  # assemble and print the tracked files consumers receive
molto package         # also compile and link a consumer of the assembled copy
```

The copy is `build/package/<name>-<version>/`. Untracked files are excluded,
uncommitted edits to tracked files produce a warning, and unmatched extra-file
patterns are errors. The consumer check ignores the author's development
configuration and catches headers which were available only in the checkout.

Existing recipe-based dependencies remain supported when their source carries
only `recipe.toml`. Libraries carrying both descriptions need a new release
which removes the recipe and declares its public interface. Molto pins immutable migrated revisions of its test frameworks until
compatible release tags are published.

See [RFC-0024](../rfcs/0024-molto-packages-as-dependencies.md) for the checklist
and ecosystem migration sequence.
