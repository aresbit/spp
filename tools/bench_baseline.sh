#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_FILE="$ROOT_DIR/bench/baseline.tsv"
CASES=""
SKIP_BUILD=0
VALID_CASES="async_pool,concurrency_map_vec,io_files,io_lock,io_net"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      if [[ -z "${2:-}" ]]; then
        echo "error: --out requires a file path" >&2
        exit 1
      fi
      OUT_FILE="$2"
      shift 2
      ;;
    --cases)
      if [[ -z "${2:-}" ]]; then
        echo "error: --cases requires a comma-separated list" >&2
        exit 1
      fi
      CASES="$2"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      echo "usage: $0 [--out <path>] [--cases <comma-separated>] [--skip-build]" >&2
      exit 1
      ;;
  esac
done

if ! command -v /usr/bin/time >/dev/null 2>&1; then
  echo "error: /usr/bin/time not found" >&2
  exit 1
fi

cd "$ROOT_DIR"

# Build and ensure test binaries exist.
if [[ "$SKIP_BUILD" -ne 1 ]]; then
  make test >/dev/null
fi

if [[ -n "$CASES" ]]; then
  IFS=',' read -r -a _requested_cases <<< "$CASES"
  for case_name in "${_requested_cases[@]}"; do
    if [[ ",$VALID_CASES," != *",$case_name,"* ]]; then
      echo "error: unknown case: $case_name" >&2
      exit 1
    fi
  done
fi

has_case() {
  local name="$1"
  if [[ -z "$CASES" ]]; then
    return 0
  fi
  local item
  IFS=',' read -r -a _case_items <<< "$CASES"
  for item in "${_case_items[@]}"; do
    if [[ "$item" == "$name" ]]; then
      return 0
    fi
  done
  return 1
}

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
  if has_case "async_pool"; then
    run_case "async_pool" "$ROOT_DIR/tests/async" "$ROOT_DIR/build/bin/tests/async/pool"
  fi
  if has_case "concurrency_map_vec"; then
    run_case "concurrency_map_vec" "$ROOT_DIR/tests/concurrency" "$ROOT_DIR/build/bin/tests/concurrency/concurrent"
  fi
  if has_case "io_files"; then
    run_case "io_files" "$ROOT_DIR/tests/io" "$ROOT_DIR/build/bin/tests/io/files"
  fi
  if has_case "io_lock"; then
    run_case "io_lock" "$ROOT_DIR/tests/io" "$ROOT_DIR/build/bin/tests/io/lock"
  fi
  if has_case "io_net"; then
    run_case "io_net" "$ROOT_DIR/tests/io" "$ROOT_DIR/build/bin/tests/io/net"
  fi
} > "$OUT_FILE"

echo "wrote baseline metrics: $OUT_FILE"
