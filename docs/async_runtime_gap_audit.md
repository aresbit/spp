# Async Runtime Gap Audit (Phase 4)

Date: 2026-04-13  
Scope: compare `docs/spp_design.md` async/runtime target vs current implementation.

## Summary

Current async stack (`Task` + `Pool` + platform `Event`) is usable and tests pass, but it is still a **minimal coroutine executor**, not yet the design-target async runtime.

Overall readiness score (Phase 4 async target): **58 / 100**

- Core coroutine task model: **good**
- Cross-platform event integration: **partial**
- Scheduler sophistication / fairness / cancellation: **weak**
- Async API consistency and lifecycle safety: **partial**

## Evidence Snapshot

- Task/promise core: [async.h](/home/ares/yyscode/spp/spp/include/spp/async/async.h)
- Thread pool runtime: [pool.h](/home/ares/yyscode/spp/spp/include/spp/async/pool.h)
- POSIX event backend: [async_pos.cpp](/home/ares/yyscode/spp/spp/src/platform/pos/async_pos.cpp)
- BSD event backend: [async_bsd.cpp](/home/ares/yyscode/spp/spp/src/platform/pos/async_bsd.cpp)
- Win32 event backend: [async_w32.cpp](/home/ares/yyscode/spp/spp/src/platform/w32/async_w32.cpp)
- Async file/timer APIs: [asyncio.h](/home/ares/yyscode/spp/spp/include/spp/async/asyncio.h), [asyncio_pos.cpp](/home/ares/yyscode/spp/spp/src/platform/pos/asyncio_pos.cpp), [asyncio_w32.cpp](/home/ares/yyscode/spp/spp/src/platform/w32/asyncio_w32.cpp), [asyncio_bsd.cpp](/home/ares/yyscode/spp/spp/src/platform/pos/asyncio_bsd.cpp)
- Existing tests: [coro.cpp](/home/ares/yyscode/spp/spp/tests/async/coro.cpp), [pool.cpp](/home/ares/yyscode/spp/spp/tests/async/pool.cpp)

## Gap Matrix

1. Async runtime with work-stealing scheduler  
Status: **Gap**  
Current: pool picks queue by "empty-first then sequence"; no stealing path.  
Impact: queue imbalance under skewed workloads.  
Priority: **P1**

2. Structured cancellation / task cancellation token  
Status: **Gap**  
Current: no cancellation token; dropped pending jobs can leak continuation ownership.  
Evidence: explicit leak note in pool destructor comment.  
Priority: **P0**

3. Awaitable channel integration with async runtime  
Status: **Gap**  
Current: MPMC exists in `concurrency/channel.h` but blocking/thread-based, not `co_await`-aware.  
Priority: **P1**

4. Event readiness fast-path parity  
Status: **Gap**  
Current: `Event::try_wait()` on Linux/macOS always returns `false`; Windows has real implementation.  
Impact: extra scheduling/wait latency and non-uniform behavior across OS.  
Priority: **P0**

5. Event wait backend efficiency  
Status: **Gap**  
Current: Linux `wait_any` creates/closes epoll fd on each wait; macOS does same with kqueue.  
Impact: high syscall churn under many events.  
Priority: **P1**

6. Async I/O API parity across POSIX variants  
Status: **Gap**  
Current: `asyncio.h` exposes `read_result/write_result`; BSD file only implements old `read/write`.  
Impact: API inconsistency and potential link/portability issue on macOS paths using Result APIs.  
Priority: **P0**

7. Runtime lifecycle safety guarantees  
Status: **Partial**  
Current: runtime shutdown works in tests, but abandoned task/continuation lifecycle is fragile (state machine + abandonment rules).  
Priority: **P1**

8. Error model richness for async subsystem  
Status: **Partial**  
Current: Result exists, but many async errors are string literals (`"open_failed"`, `"read_failed"`) with limited domain typing.  
Priority: **P2**

9. Observability/metrics for scheduler and queue depth  
Status: **Gap**  
Current: no runtime stats API (queue depth, wakeups, event loop lag, steals).  
Priority: **P2**

## What Is Already Strong

1. `Task`/coroutine primitives are clean and low-overhead in no-exception mode.
2. `Pool` integration works for CPU-bound coroutine fan-out workloads.
3. Async `wait` and async file APIs exist on Linux/Windows and are test-covered indirectly by pool flow.
4. Compatibility strategy is practical (`*_result` alongside old wrappers in adjacent modules).

## Recommended Execution Plan (Compatibility-Safe)

1. P0.1 Implement POSIX `Event::try_wait()` parity (`poll`/`kevent` non-blocking check).
2. P0.2 Add BSD `read_result/write_result` implementations to match `asyncio.h`.
3. P0.3 Add lifecycle safety test that stresses task drop/shutdown/continuation ordering.
4. P1.1 Introduce cancellation token (opt-in) for `Task` and `Pool::suspend/event`.
5. P1.2 Add basic work stealing path (victim queue pop when local empty).
6. P1.3 Add async-aware channel awaiters (start with MPMC recv awaitable).

## Proposed Acceptance Criteria for Next Async Milestone

1. Cross-platform build parity for `Async::read_result/write_result`.
2. `Event::try_wait()` semantic parity on Linux/macOS/Windows.
3. New stress tests pass:
   - task abandonment lifecycle
   - shutdown with pending event jobs
   - high-contention scheduler fairness.
4. No regressions in existing `make test`.
