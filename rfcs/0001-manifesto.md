# RFC 0001: Manifesto

- RFC Number: 0001
- Title: Manifesto
- Status: Accepted
- Created: 2026-07-26

## Summary

This RFC establishes the founding vision, goals, non-goals, philosophy, and
design principles of Molto. As the first accepted RFC, it is immutable: new
behavior must be introduced through new RFCs, per the process defined in
`spec.md`.

## Vision

Molto is a modern packaging ecosystem for C and C++.

Its purpose is not to replace GCC, Clang, or MSVC, but to modernize the
developer experience around them. Molto provides project management,
dependency management, reproducible builds, incremental compilation, a
package registry, a plugin ecosystem, workspace metadata, and project
templates, while remaining compiler agnostic.

## Goals

Molto should make C and C++ development feel similar to Rust's Cargo while
preserving the flexibility and philosophy of native toolchains.

The project prioritizes:

- Convention over configuration
- Fast builds
- Zero configuration for common projects
- Cross-platform support
- Excellent compiler diagnostics
- Reproducible builds
- Extensible architecture

## Non-Goals

Molto is NOT:

- a compiler
- a linker
- a build language
- a replacement for GCC
- a replacement for Clang
- a replacement for MSVC

Molto orchestrates existing toolchains; it does not replace them.

## Philosophy

Configuration should be optional. The filesystem should describe the project
whenever possible.

Example:

```
src/
controllers/
repository/
models/
tests/
bench/
```

No file lists are required. Molto discovers source files automatically.

## Design Principles

Every feature must satisfy:

- Simple by default
- Explicit when needed
- Fast
- Portable
- Deterministic
- Backwards compatible whenever possible
