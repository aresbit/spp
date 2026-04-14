# SPP Task Plan (Execution-Strict)

Last updated: 2026-04-14
Scope baseline: `docs/spp_design.md`

## Execution Rules
- One feature per commit.
- Every step must be test-verified (`make test`) before commit.
- All progress is recorded in this file before starting next step.
- New feature work must include explicit acceptance criteria and targeted test mapping.

## Plan Quality Evals (Autoresearch)
- `eval_1_requirement_traceability`: Every active phase item maps to at least one design requirement.
- `eval_2_feat_granularity`: Remaining work is split into feat-sized units with clear ownership scope.
- `eval_3_test_gate`: Every planned feat has explicit test verification gate.
- `eval_4_acceptance_binary`: Every planned feat has binary acceptance criteria.
- `eval_5_priority_order`: Remaining work is ordered by P0/P1/P2 impact.
- `eval_6_platform_parity`: Async/IO backlog calls out Linux/macOS/Windows parity.
- `eval_7_commit_hygiene`: History section distinguishes done vs planned, no stale pending duplication.
- `eval_8_next_step_actionable`: Immediate next steps can be executed without extra clarification.
- `eval_9_phase_status_consistency`: Phase status lines match checklist completion.
- `eval_10_design_alignment`: Functional and non-functional requirements are both represented.

## Feature Requirement Coverage Matrix
| Requirement | Design intent | Planned/Delivered feature coverage | Status |
|---|---|---|---|
| FR-1 Memory safety | Ownership + lifetime correctness | Phase 1 allocator/ownership hardening; Phase 4 concurrent safety pass | covered |
| FR-2 Type safety | ADT + explicit result/option model | Phase 2 Result migration + Phase 3 pattern match alignment | covered |
| FR-3 Functional style | composable APIs, immutable defaults | Phase 3 combinators and immutable behavior audit | covered |
| FR-4 Zero-cost abstraction | no runtime tax for safety abstractions | Phase 5 perf/memory milestone pending benchmarking | partial |
| FR-5 Allocator awareness | allocator-parametric containers | Phase 1 cross-allocator clone/lifecycle fixes | covered |
| FR-6 Concurrency safety | safe runtime/channel behavior | Phase 4 async APIs + concurrent container safety pass | covered |
| FR-7 Interoperability | C++ migration-friendly APIs | compatibility wrappers in Phase 2/4/5 | covered |
| FR-8 Reflection/serialization | compile-time reflection + json path | Phase 5 serialization/reflection integration delivered | covered |
| FR-9 Error handling | Result-first error path | Phase 2 + async/io `*_result` APIs | covered |
| FR-10 Portability/testability | multi-platform + stable test gate | cross-backend async APIs + pending parity/consistency hardening | partial |

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
- Status: completed

### Phase 2: String/Result Migration
- [x] Introduce `Result<T, E>` core type
- [x] Add `Files::*_result` APIs with compatibility wrappers
- [x] Add `Async::*_result` APIs with compatibility wrappers
- [x] Migrate test harness (`tests/test.h`) to Result-based file path
- [x] Migrate string/parse path (`reflection/format1`) from `Opt` to `Result`
- [x] Migrate network receive/error path to `Result`
- Status: completed

### Phase 3: Functional Layer Alignment
- [x] Result combinators (`map`, `map_err`, `and_then`, `or_else`)
- [x] Pattern matching alignment and API cleanup
- [x] Immutable collection behavior audit vs design doc
- Status: completed

### Phase 4: Concurrency & Async Alignment
- [x] Channel abstractions
- [x] Async runtime capability gap audit vs design doc
- [x] Result-based async wait API (`wait_result`) across backends
- [x] Async channel timeout APIs (`recv_for/send_for`)
- [x] Pool runtime stats API (`Pool::stats`)
- [x] Timed event wait API (`Event::wait_any_for`)
- [x] Cancellable wait API (`Cancel_Token` + `wait_result(..., token)`)
- [x] Concurrent container safety pass
- Status: completed

### Phase 5: Ecosystem & Optimization
- [x] Serialization / reflection integration checkpoints
- [x] Network and IO API consistency pass
- [x] Performance and memory optimization milestones
- Status: completed

## Remaining Feat Backlog (Autoresearch 5-Round Target)

### FEAT-401 (P0) Concurrent Container Safety Pass
- Scope: lock/atomic invariants, ownership handoff, iterator invalidation rules across concurrent containers.
- Acceptance (binary):
  - [x] race-prone paths audited and fixed with explicit invariants.
  - [x] new/updated concurrency stress tests pass in `tests/concurrency/*`.
  - [x] `make test` passes.
- Verification: `make test` + focused concurrency suite.

### FEAT-402 (P0) Async/IO API Consistency Across Backends
- Scope: align POSIX/BSD/Win32 IO result APIs, timeout semantics, and error strings.
- Acceptance (binary):
  - [x] `read_result/write_result` parity across supported backends.
  - [x] timeout/cancel semantics documented and test-covered.
  - [x] `make test` passes on current platform with parity guards for others.
- Verification: `make test` + async/io regression cases.

### FEAT-403 (P1) Event Backend Efficiency & Fairness Hardening
- Scope: reduce per-wait syscall churn, tighten wake-up fairness and queue balance evidence.
- Acceptance (binary):
  - [x] backend wait path avoids repeated expensive fd lifecycle where feasible.
  - [x] fairness regression test demonstrates no starvation in synthetic contention.
  - [x] `make test` passes.
- Verification: `make test` + added fairness test.

### FEAT-404 (P1) Typed Async Error Model Refinement
- Scope: reduce fragile string-literal errors in async/IO path; introduce typed/domain-tagged errors where viable.
- Acceptance (binary):
  - [x] core async errors converted from ad-hoc literals to typed or normalized domain codes.
  - [x] compatibility wrappers preserved.
  - [x] `make test` passes.
- Verification: `make test` + error-path unit coverage.

### FEAT-405 (P2) Perf/Memory Milestone Baseline
- Scope: benchmark hooks for container/async hot paths and memory overhead checkpoints.
- Acceptance (binary):
  - [x] reproducible baseline command documented.
  - [x] at least one perf and one memory metric tracked in repo docs.
  - [x] no functional regressions (`make test`).
- Verification: `make test` + benchmark script/check commands.

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

## Planned Next Feat Commits (In Order)
1. `chore`: add baseline update checklist for bench drift management.
2. `chore`: split CI bench jobs by case group for clearer regression ownership.

## Immediate Next Step
1. Document baseline update review checklist in docs.
2. Add explicit ownership for each benchmark case in task plan.
