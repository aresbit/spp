# Autoresearch Changelog (SPP System Library)

## Baseline Setup
- Target: `/home/ares/yyscode/spp/spp` async/runtime feature completeness vs `docs/spp_design.md`
- Runs: 5
- Strategy: feature-first mutations (one feature per commit)

## Experiment 1 — keep
- Commit: `74f21c2`
- Change: add `Async::wait_result(Pool&, u64)` parity API across Linux/BSD/Windows
- Result: `make test` pass

## Experiment 2 — keep
- Commit: `23a08c7`
- Change: add async channel timeout APIs `recv_for/send_for`
- Result: `make test` pass

## Experiment 3 — keep
- Commit: `e75acc4`
- Change: add `Async::Pool::stats()` runtime counters
- Result: `make test` pass

## Experiment 4 — keep
- Commit: `83dffec`
- Change: add timed `Event::wait_any_for(...)` API across backends
- Result: `make clean && make test` pass

## Experiment 5 — keep
- Commit: `c361b3f`
- Change: add `Cancel_Token` and cancellable `wait_result(..., token)`
- Result: `make test` pass
