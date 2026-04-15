# SPP Module Roadmap

## Core

- Ownership: `Box`, `Rc`, `Arc`, `Ref`
- Error model: `Result<T, E>`, `Opt<T>`
- Deterministic primitives: decimal/time
- Allocators: default/region/pool and parameterized containers

## Containers

- `Vec<T, A>` base growth policy and safety checks
- `Map<K, V, A>` lifetime correctness and iterator safety
- String and slice utilities with zero-copy views where possible

## Concurrency and Async

- Lock-free ring/message bus
- Channels and thread pool behavior
- Runtime scheduling and safe callback interfaces

## I/O and Platform

- Unified `IO::Handle` abstraction for file/socket/pipe
- POSIX + Windows parity for core APIs
- Direct I/O and async paths where available

## Reflection and Serialization

- Stable binary encoding/decoding
- Schema evolution support
- Persistence helpers and deterministic format behavior
