# Perf & Memory Baseline (FEAT-405)

## Reproducible Command

From repository root:

```bash
./tools/bench_baseline.sh
```

The script compiles test binaries and writes baseline metrics to:

- `bench/baseline.tsv`

To run the regression gate against baseline:

```bash
./tools/bench_check.sh
```

## Metrics

The baseline tracks one performance metric and one memory metric per case:

- `elapsed_sec`: wall-clock execution time (seconds)
- `max_rss_kb`: peak resident set size in KB

## Covered Cases

- `async_pool`: async runtime scheduling and wait-path stress
- `concurrency_map_vec`: concurrent container mixed operations
- `io_files`: file IO read/write and metadata operations
- `io_net`: TCP bind/listen/connect/send/recv path

## Notes

- Measurements are collected via `/usr/bin/time`.
- Keep machine load stable when comparing baselines.
- Re-run `./tools/bench_baseline.sh` after major runtime/container changes and review deltas in `bench/baseline.tsv`.
- Allowed regressions are configured in `bench/thresholds.tsv`.
- CI runs `make bench-check` and fails on threshold breach.
