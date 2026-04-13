# SPP Task Plan (Execution-Strict)

Last updated: 2026-04-13
Scope baseline: `docs/spp_design.md`

## Execution Rules
- One feature per commit.
- Every step must be test-verified (`make test`) before commit.
- All progress is recorded in this file before starting next step.

## Phase Progress

### Phase 1: Core Foundation (Ownership / Allocator / Basic Containers)
- [x] Bootstrap from RPP and integrate modern-c-makefile build/test
- [x] Restructure to modern-c-makefile layout
- [x] Split module directories (`include/spp/*`, `src/*`, `tests/*`)
- [x] Box/Rc/Arc allocator parameterization hardening
- [x] Vec allocator cross-clone support + lifecycle fix
- [x] Queue/Heap/Map allocator cross-clone support
- [x] Queue/Heap lifecycle fix after reserve/move
- [x] Map slot lifecycle fix during rehash
- Status: completed (for current baseline)

### Phase 2: String/Result Migration (Current Focus)
- [x] Introduce `Result<T, E>` core type
- [x] Add `Files::*_result` APIs with compatibility wrappers
- [x] Add `Async::*_result` APIs with compatibility wrappers
- [x] Migrate test harness (`tests/test.h`) to Result-based file path
- [x] Migrate string/parse path (`reflection/format1`) from `Opt` to `Result`
- [x] Migrate network receive/error path to `Result`
- Status: in progress

### Phase 3: Functional Layer Alignment
- [x] Result combinators (`map`, `map_err`, `and_then`, `or_else`)
- [x] Pattern matching alignment and API cleanup
- [x] Immutable collection behavior audit vs design doc
- Status: completed (for current baseline)

### Phase 4: Concurrency & Async Alignment
- [x] Channel abstractions
- [x] Async runtime capability gap audit vs design doc
- [x] Result-based async wait API (`wait_result`) across backends
- [x] Async channel timeout APIs (`recv_for/send_for`)
- [x] Pool runtime stats API (`Pool::stats`)
- [x] Timed event wait API (`Event::wait_any_for`)
- [x] Cancellable wait API (`Cancel_Token` + `wait_result(..., token)`)
- [ ] Concurrent container safety pass
- Status: in progress

### Phase 5: Ecosystem & Optimization
- [x] Serialization / reflection integration checkpoints
- [ ] Network and IO API consistency pass
- [ ] Performance and memory optimization milestones
- Status: in progress

## Completed Commits (Trace)
- `e2f5836` feat: add json serialization support for option and result
- `e513771` feat: add json parse_result for primitives and vectors
- `b67e113` feat: add pretty json output for reflection serialization
- `f97c104` feat: add json serialization for vec slice array and map
- `b68d6ee` feat: add reflection-driven json serialization core
- `c361b3f` feat: add cancellable async wait with cancel token
- `83dffec` feat: add timed wait_any_for event API across backends
- `e75acc4` feat: add async pool runtime stats counters
- `23a08c7` feat: add async channel timeout send and recv APIs
- `74f21c2` feat: add Result-based async wait API across platforms
- `8187391` feat: add async await helpers for mpmc channel
- `b924db5` feat: add basic work-stealing path to async pool
- `e9ab384` feat: add non-blocking event try_wait on posix backends
- `39d849b` feat: align bsd asyncio with Result read/write APIs
- `333b187` migrate file and async io APIs toward Result
- `pending` add async runtime capability gap audit document
- `pending` add MPMC channel baseline (`Sender`/`Receiver`)
- `pending` migrate udp recv path toward Result (with compatibility wrapper)
- `pending` add free `match(...)` API for Variant
- `pending` add const/non-mutating lookup path for string-keyed Map
- `pending` add Result combinators (`map/map_err/and_then/or_else`)
- `pending` migrate format1 parse APIs toward Result (with compatibility wrappers)
- `f353320` add core Result type for explicit error handling
- `829345c` fix map slot move lifecycle during rehash
- `2ec6ade` fix queue and heap reserve destruction semantics
- `8960954` enable cross-allocator clone for queue heap map
- `174befd` add cross-allocator clone for rc and arc
- `a4bee0d` enable cross-allocator box clone
- `927c4eb` add cross-allocator vec clone coverage
- `fea41cb` fix vec move-reserve destruction semantics
- `0ad67d3` split tests into module subdirectories
- `0f3fc02` split src into module/platform subdirectories
- `ad6f0c4` split include tree into module subdirectories
- `6b4a525` reorganize to modern-c-makefile directory layout
- `4f3cab2` bootstrap spp from rpp and integrate tests into make

## Immediate Next Step
1. Complete concurrent container safety pass (Phase 4 remaining unchecked item).
2. Continue Phase 5 network and IO API consistency pass.
3. Continue one-feature-per-commit with `make test` gate.
