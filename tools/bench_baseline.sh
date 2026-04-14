#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_FILE="$ROOT_DIR/bench/baseline.tsv"

if [[ "${1:-}" == "--out" ]]; then
  if [[ -z "${2:-}" ]]; then
    echo "error: --out requires a file path" >&2
    exit 1
  fi
  OUT_FILE="$2"
elif [[ -n "${1:-}" ]]; then
  echo "error: unknown argument: $1" >&2
  echo "usage: $0 [--out <path>]" >&2
  exit 1
fi

if ! command -v /usr/bin/time >/dev/null 2>&1; then
  echo "error: /usr/bin/time not found" >&2
  exit 1
fi

cd "$ROOT_DIR"

# Build and ensure test binaries exist.
make test >/dev/null

run_case() {
  local name="$1"
  local workdir="$2"
  shift
  shift
  local tmp
  tmp="$(mktemp)"

  (
    cd "$workdir"
    /usr/bin/time -f '%e\t%M' -o "$tmp" "$@" >/dev/null
  )

  local elapsed
  local rss
  IFS=$'\t' read -r elapsed rss < "$tmp"
  rm -f "$tmp"

  printf '%s\t%s\t%s\n' "$name" "$elapsed" "$rss"
}

{
  printf 'case\telapsed_sec\tmax_rss_kb\n'
  run_case "async_pool" "$ROOT_DIR/tests/async" "$ROOT_DIR/build/bin/tests/async/pool"
  run_case "concurrency_map_vec" "$ROOT_DIR/tests/concurrency" "$ROOT_DIR/build/bin/tests/concurrency/concurrent"
} > "$OUT_FILE"

echo "wrote baseline metrics: $OUT_FILE"
