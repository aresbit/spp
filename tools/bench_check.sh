#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE_FILE="$ROOT_DIR/bench/baseline.tsv"
THRESHOLDS_FILE="$ROOT_DIR/bench/thresholds.tsv"
MAX_ELAPSED_REGRESS_PCT_DEFAULT="100"
MAX_RSS_REGRESS_PCT_DEFAULT="50"

if [[ -n "${1:-}" ]]; then
  echo "error: bench_check.sh does not take positional arguments" >&2
  exit 1
fi

if [[ ! -f "$BASELINE_FILE" ]]; then
  echo "error: missing baseline file: $BASELINE_FILE" >&2
  exit 1
fi

if [[ ! -f "$THRESHOLDS_FILE" ]]; then
  echo "error: missing threshold file: $THRESHOLDS_FILE" >&2
  exit 1
fi

CURRENT_FILE="$(mktemp)"
trap 'rm -f "$CURRENT_FILE"' EXIT

"$ROOT_DIR/tools/bench_baseline.sh" --out "$CURRENT_FILE" >/dev/null

echo "bench check against baseline: $BASELINE_FILE"
echo "thresholds: $THRESHOLDS_FILE"

awk -F '\t' \
  -v default_elapsed="$MAX_ELAPSED_REGRESS_PCT_DEFAULT" \
  -v default_rss="$MAX_RSS_REGRESS_PCT_DEFAULT" '
function fail(msg) {
  print "error: " msg > "/dev/stderr"
  failed = 1
}

function pct_delta(base, curr,   d) {
  if (base == 0) {
    return curr == 0 ? 0 : 1000000
  }
  d = ((curr - base) / base) * 100.0
  return d
}

FILENAME == ARGV[1] {
  if ($1 == "case") next
  base_elapsed[$1] = $2 + 0.0
  base_rss[$1] = $3 + 0.0
  seen_base[$1] = 1
  next
}

FILENAME == ARGV[2] {
  if ($1 == "case") next
  th_elapsed[$1] = $2 + 0.0
  th_rss[$1] = $3 + 0.0
  next
}

FILENAME == ARGV[3] {
  if ($1 == "case") next
  case_name = $1
  curr_elapsed = $2 + 0.0
  curr_rss = $3 + 0.0
  seen_current[case_name] = 1

  if (!(case_name in base_elapsed)) {
    fail("case " case_name " exists in current run but not in baseline")
    next
  }

  base_e = base_elapsed[case_name]
  base_m = base_rss[case_name]
  d_e = pct_delta(base_e, curr_elapsed)
  d_m = pct_delta(base_m, curr_rss)

  max_e = (case_name in th_elapsed) ? th_elapsed[case_name] : default_elapsed
  max_m = (case_name in th_rss) ? th_rss[case_name] : default_rss

  status = "PASS"
  if (d_e > max_e || d_m > max_m) {
    status = "FAIL"
    fail(sprintf("case %s regressed: elapsed %+0.2f%% (max %+0.2f%%), rss %+0.2f%% (max %+0.2f%%)", case_name, d_e, max_e, d_m, max_m))
  }

  printf "%-20s elapsed: %8.4fs -> %8.4fs (%+7.2f%%, max %+6.2f%%) | rss: %7.0fKB -> %7.0fKB (%+7.2f%%, max %+6.2f%%) [%s]\n",
    case_name, base_e, curr_elapsed, d_e, max_e, base_m, curr_rss, d_m, max_m, status
}

END {
  for (c in seen_base) {
    if (!(c in seen_current)) {
      fail("case " c " exists in baseline but not in current run")
    }
  }
  exit failed ? 1 : 0
}
' "$BASELINE_FILE" "$THRESHOLDS_FILE" "$CURRENT_FILE"

echo "bench check passed"
