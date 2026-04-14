#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE_FILE="$ROOT_DIR/bench/baseline.tsv"
THRESHOLDS_FILE="$ROOT_DIR/bench/thresholds.tsv"
MAX_ELAPSED_REGRESS_PCT_DEFAULT="100"
MAX_RSS_REGRESS_PCT_DEFAULT="50"
MAX_ELAPSED_REGRESS_ABS_SEC_DEFAULT="0.02"
MAX_RSS_REGRESS_ABS_KB_DEFAULT="512"
CURRENT_OUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --current-out)
      if [[ -z "${2:-}" ]]; then
        echo "error: --current-out requires a file path" >&2
        exit 1
      fi
      CURRENT_OUT="$2"
      shift 2
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      echo "usage: $0 [--current-out <path>]" >&2
      exit 1
      ;;
  esac
done

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

if [[ -n "$CURRENT_OUT" ]]; then
  mkdir -p "$(dirname "$CURRENT_OUT")"
  cp "$CURRENT_FILE" "$CURRENT_OUT"
fi

echo "bench check against baseline: $BASELINE_FILE"
echo "thresholds: $THRESHOLDS_FILE"

awk -F '\t' \
  -v default_elapsed="$MAX_ELAPSED_REGRESS_PCT_DEFAULT" \
  -v default_rss="$MAX_RSS_REGRESS_PCT_DEFAULT" \
  -v default_elapsed_abs="$MAX_ELAPSED_REGRESS_ABS_SEC_DEFAULT" \
  -v default_rss_abs="$MAX_RSS_REGRESS_ABS_KB_DEFAULT" '
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
  th_elapsed_abs[$1] = ($4 == "" ? default_elapsed_abs + 0.0 : $4 + 0.0)
  th_rss_abs[$1] = ($5 == "" ? default_rss_abs + 0.0 : $5 + 0.0)
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
  d_e_abs = curr_elapsed - base_e
  d_m_abs = curr_rss - base_m

  max_e = (case_name in th_elapsed) ? th_elapsed[case_name] : default_elapsed
  max_m = (case_name in th_rss) ? th_rss[case_name] : default_rss
  max_e_abs = (case_name in th_elapsed_abs) ? th_elapsed_abs[case_name] : default_elapsed_abs
  max_m_abs = (case_name in th_rss_abs) ? th_rss_abs[case_name] : default_rss_abs

  status = "PASS"
  elapsed_regress = (d_e > max_e && d_e_abs > max_e_abs)
  rss_regress = (d_m > max_m && d_m_abs > max_m_abs)

  if (elapsed_regress || rss_regress) {
    status = "FAIL"
    fail(sprintf("case %s regressed: elapsed %+0.2f%% / %+0.4fs (max %+0.2f%% or %+0.4fs), rss %+0.2f%% / %+0.0fKB (max %+0.2f%% or %+0.0fKB)",
      case_name, d_e, d_e_abs, max_e, max_e_abs, d_m, d_m_abs, max_m, max_m_abs))
  }

  printf "%-20s elapsed: %8.4fs -> %8.4fs (%+7.2f%%, %+8.4fs; max %+6.2f%% or %+8.4fs) | rss: %7.0fKB -> %7.0fKB (%+7.2f%%, %+7.0fKB; max %+6.2f%% or %+7.0fKB) [%s]\n",
    case_name, base_e, curr_elapsed, d_e, d_e_abs, max_e, max_e_abs, base_m, curr_rss, d_m, d_m_abs, max_m, max_m_abs, status
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
