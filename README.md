# Molto

A modern packaging ecosystem for C and C++. See [`spec.md`](spec.md) and the
[RFCs](rfcs/) for the design.

This repository contains the `molto` CLI. It is written in C and, for now,
bootstrapped with a plain `Makefile` (molto cannot build itself yet).

## Requirements

- `gcc-12` or newer (the build targets the C23 subset via `-std=c2x`)
- GNU Make

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

## Project layout

```
include/molto/   Public headers (used as <molto/...>)
src/             CLI entry point, command handlers, and services
tests/           Minimal test framework and suites
```
