# SPP Project Integration

## 1) Build Integration

- Keep a clear app-vs-library boundary.
- Pin C++ standard and core flags consistently across app and SPP.
- Enforce no-exception/no-RTTI mode consistently if app policy follows SPP runtime behavior.

## 2) API Design in App Layer

- Prefer returning `Result<T, E>` from service/data boundaries.
- Keep ownership in type signatures (`Box/Rc/Arc`).
- Pass read-only views using `Slice`/`String_View` where possible.

## 3) Performance and Safety

- Select allocator strategy for high-throughput modules.
- Use debug assertions/tests for bounds and contract checks.
- Add targeted microbench for critical queues/serialization paths.

## 4) Persistence/Data Wire

- Use stable binary codec for disk/network protocol boundaries.
- Include schema/version fields in persisted payloads.
- Test forward/backward compatibility for evolution.

## 5) Test Strategy

- Unit tests: API semantics and error branches.
- Integration tests: I/O + codec + concurrency paths.
- Stress tests for lock-free or async paths under contention.
