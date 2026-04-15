---
name: spp-skill
description: "Guide external users to build projects on top of SPP (not SPP internals): scaffold app layout, wire modern-c-makefile builds, use SPP ownership/containers/concurrency/I-O APIs, and validate with make test. Use when creating a new SPP-based application, migrating std-heavy code to SPP, or troubleshooting SPP usage in app code."
---

# SPP Skill

## Goal

Build production application code with SPP APIs and patterns while keeping explicit ownership, allocator-aware containers, and `Result`-based error handling.

## Quick Start

1. Create project skeleton:
- `include/` for public app headers
- `src/` for implementation
- `tests/` for behavior tests
- `Makefile` using modern-c-makefile style targets

2. Add SPP include path and link settings:
- Include headers from `spp/include`
- Link against built SPP static/shared library as configured in your Makefile

3. Write app code with SPP primitives:
- Ownership: `Box`, `Rc`, `Arc`, `Ref`
- Containers: `Vec`, `Map`, `String`/`String_View`, `Slice`
- Error flow: `Result<T, E>`, `Opt<T>`
- Concurrency: channels, thread/pool, lock-free ring where needed
- I/O: `IO::Handle`-style unified file/socket paths when available

4. Validate:
- `make test`
- Add targeted tests for API behavior and failure-path `Result` handling

## External Project Rules

- Prefer SPP types in domain/core modules; isolate unavoidable `std` interop at boundaries.
- Keep API ownership explicit in function signatures.
- Never rely on exceptions for control flow; return `Result`.
- Avoid hidden allocations in hot path; keep allocator choice explicit for critical containers.
- Use stable binary/persistence codec helpers for durable on-disk data.

## Migration Recipe (std -> SPP)

1. Replace pointer ownership first (`unique_ptr/shared_ptr` -> `Box/Rc/Arc`).
2. Replace containers in hot paths (`std::vector/map/string` -> `Vec/Map/String`).
3. Convert throw/catch boundaries to `Result` returns.
4. Add regression tests before and after each module migration.
5. Bench micro-hotspots after functional parity.

## Build and Debug Commands

```bash
# app repo root
make test

# search migration points
rg -n "std::|throw|catch|unique_ptr|shared_ptr|vector|map|string" include src tests

# find SPP usage footprint
rg -n "spp::|Box<|Rc<|Arc<|Result<|Vec<|Map<" include src tests
```

## References

- App integration checklist: `references/project-integration.md`
