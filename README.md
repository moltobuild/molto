# Molto

A modern packaging ecosystem for C and C++.

- [`docs/Project.md`](docs/Project.md) — how to configure `Project.toml` to
  build a C/C++ project: include paths, link libraries, test layout, profiles,
  and what to do when a build that works with `make` does not work here
- [`docs/Style.md`](docs/Style.md) — `molto fmt` and `molto lint`: the two
  configuration files, where the formatter and the linter come from, and what
  each command reports
- [`docs/Metadata.md`](docs/Metadata.md) — `molto metadata`: the CycloneDX bill
  of materials, what it contains, and why it carries no timestamp
- [`spec.md`](spec.md) and the [RFCs](rfcs/) — the design

This repository contains the `molto` CLI. It is written in C and **builds
itself**:

```sh
molto build      # produces build/debug/molto
molto test       # builds and runs the suite as one executable
molto build -j 4 # the same, on four cores instead of all of them
```

Every build also writes `compile_commands.json` at the project root, so clangd,
clang-tidy and anything else that parses this code reads the flags the build
actually used. Nothing to enable; `molto test` writes one covering `tests/` too.

The `Makefile` remains the bootstrap — something has to compile the first
molto — but it is no longer the only way. Its manifest names no compiler: it
states the features the code needs and pickup resolves them.

## Install

A static x86-64 Linux binary is attached to every
[release](https://github.com/black-redoc/molto/releases). It carries no glibc
requirement and there is nothing to unpack.

```sh
base=https://github.com/black-redoc/molto/releases/latest/download
curl -fsSLO $base/SHA256SUMS
curl -fsSLO $base/molto-0.16.0-x86_64-linux
sha256sum --check --ignore-missing SHA256SUMS
sudo install molto-0.16.0-x86_64-linux /usr/local/bin/molto
```

**Then name a compiler, for now.** Molto does not choose one: it asks `pickup`,
and pickup has no release yet. Until it does:

```sh
export C_COMPILER=gcc CPP_COMPILER=g++
```

Without that, `new`, `init`, `add`, `remove` and `metadata` work, and `build`,
`test`, `fmt` and `lint` report that no compiler could be resolved. Each release
also carries `molto-<version>.cdx.json`, molto's own CycloneDX bill of
materials.

## Requirements

To build molto from source:

- `gcc-12` or newer (the build targets the C23 subset via `-std=c2x`)
- GNU Make
- [`pickup`](../pickup), the toolchain manager: Molto asks it which compiler
  satisfies a project's `[target]` requirements. Point `MOLTO_PICKUP` at it if
  it is not on the `PATH`, or set `C_COMPILER` / `CPP_COMPILER` to choose the
  drivers by hand.

Once a modern toolchain (GCC 14 / Clang 19) is available, bump `STD` to `c23`
and `CC` accordingly in the `Makefile`.

## Build

```sh
make build        # produces build/molto
```

## Run

```sh
make run ARGS="--help"
make run ARGS="new my_app"
./build/molto new my_app
```

## Test

```sh
make test
```

## Licence

Apache License 2.0 — see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). The patent
grant is the reason for choosing it over a shorter permissive licence: Molto
runs other people's compilers and its registry distributes other people's code.

## Project layout

```
include/molto/   Public headers (used as <molto/...>)
src/             CLI entry point, command handlers, and services
tests/           The test suites
modules/moltest/ The test framework, a standalone module
docs/            User guides (start with docs/Project.md)
rfcs/            Design documents
```
