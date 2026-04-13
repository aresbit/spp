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
- [ ] Result combinators (`map`, `map_err`, `and_then`, `or_else`)
- [ ] Pattern matching alignment and API cleanup
- [ ] Immutable collection behavior audit vs design doc
- Status: pending

### Phase 4: Concurrency & Async Alignment
- [ ] Channel abstractions
- [ ] Async runtime capability gap audit vs design doc
- [ ] Concurrent container safety pass
- Status: pending

### Phase 5: Ecosystem & Optimization
- [ ] Serialization / reflection integration checkpoints
- [ ] Network and IO API consistency pass
- [ ] Performance and memory optimization milestones
- Status: pending

## Completed Commits (Trace)
- `333b187` migrate file and async io APIs toward Result
- `pending` migrate udp recv path toward Result (with compatibility wrapper)
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
1. Implement `Result` combinators in `include/spp/core/result.h`: `map`, `map_err`, `and_then`, `or_else`.
2. Add dedicated tests in `tests/core/result.cpp`.
3. `make test` and commit as one feature.
