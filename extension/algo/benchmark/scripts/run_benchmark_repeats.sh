#!/usr/bin/env bash
set -euo pipefail

DEFAULT_BENCH_BIN="./build/stree-linux-relwithdebinfo/extension/algo/test/dynamic_connectivity_benchmark"
BENCH_BIN="${BENCH_BIN:-$DEFAULT_BENCH_BIN}"

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "Usage:"
  echo "  BENCH_BIN=<path-to-dynamic_connectivity_benchmark> $0 <trace_file> <num_runs> [--validate=expected|--validate=none]"
  echo
  echo "Example:"
  echo "  BENCH_BIN=$DEFAULT_BENCH_BIN \\"
  echo "    $0 extension/algo/benchmark/traces/tiny_sanity.updates 10 --validate=expected"
  echo
  echo "If BENCH_BIN is not set, the script uses:"
  echo "  $DEFAULT_BENCH_BIN"
  exit 1
fi

TRACE_FILE="$1"
NUM_RUNS="$2"
VALIDATE_MODE="${3:---validate=none}"

if [ ! -x "$BENCH_BIN" ]; then
  echo "Error: benchmark binary not found or not executable: $BENCH_BIN"
  echo "Please build it first, for example:"
  echo "  cmake --build build/stree-linux-relwithdebinfo --target dynamic_connectivity_benchmark"
  echo "Or set BENCH_BIN explicitly:"
  echo "  BENCH_BIN=<path-to-binary> $0 <trace_file> <num_runs> [--validate=expected|--validate=none]"
  exit 1
fi

if [ ! -f "$TRACE_FILE" ]; then
  echo "Trace file not found:"
  echo "  $TRACE_FILE"
  exit 1
fi

TRACE_BASE="$(basename "$TRACE_FILE")"
TRACE_NAME="${TRACE_BASE%.*}"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

RESULT_DIR="extension/algo/benchmark/results/${TRACE_NAME}_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

SUMMARY_CSV="${RESULT_DIR}/summary.csv"

echo "run,order_in_run,method,trace_file,validate_mode,total_lines,processed_ops,skipped_lines,num_nodes,insert_count,insert_total_seconds,insert_avg_us,delete_count,delete_total_seconds,delete_avg_us,validation_checks,validation_errors,validation_total_seconds,validation_avg_us,exit_code,raw_log" > "$SUMMARY_CSV"

extract_value_after_colon() {
  local key="$1"
  local file="$2"
  grep -m 1 "$key" "$file" | sed 's/.*: //' | awk '{print $1}'
}

extract_section_value() {
  local section="$1"
  local key="$2"
  local file="$3"
  awk -v section="$section" -v key="$key" '
    $0 ~ section {in_section=1; next}
    /^[A-Za-z]+:$/ && in_section==1 {exit}
    in_section==1 && $0 ~ key {
      sub(/.*: /, "", $0);
      print $1;
      exit;
    }
  ' "$file"
}

echo "Running benchmark..."
echo "Trace file: $TRACE_FILE"
echo "Runs per method: $NUM_RUNS"
echo "Validation mode: $VALIDATE_MODE"
echo "Result directory: $RESULT_DIR"
echo


for run in $(seq 1 "$NUM_RUNS"); do
  if (( run % 2 == 1 )); then
    METHODS=("stree" "dtree")
  else
    METHODS=("dtree" "stree")
  fi

  order_in_run=0
  for method in "${METHODS[@]}"; do
    order_in_run=$((order_in_run + 1))

    RAW_LOG="${RESULT_DIR}/${method}_run_${run}.log"

    echo "Run ${run}/${NUM_RUNS}: ${method}"

    set +e
    "$BENCH_BIN" "$method" "$TRACE_FILE" "$VALIDATE_MODE" > "$RAW_LOG" 2>&1
    EXIT_CODE=$?
    set -e

  TOTAL_LINES="$(extract_value_after_colon "Total lines:" "$RAW_LOG" || echo "")"
  PROCESSED_OPS="$(extract_value_after_colon "Processed operations:" "$RAW_LOG" || echo "")"
  SKIPPED_LINES="$(extract_value_after_colon "Skipped lines:" "$RAW_LOG" || echo "")"
  NUM_NODES="$(extract_value_after_colon "Number of nodes created:" "$RAW_LOG" || echo "")"

  INSERT_COUNT="$(extract_section_value "Insertions:" "Count:" "$RAW_LOG" || echo "")"
  INSERT_TOTAL="$(extract_section_value "Insertions:" "Total time:" "$RAW_LOG" || echo "")"
  INSERT_AVG="$(extract_section_value "Insertions:" "Avg time/op:" "$RAW_LOG" || echo "")"

  DELETE_COUNT="$(extract_section_value "Deletions:" "Count:" "$RAW_LOG" || echo "")"
  DELETE_TOTAL="$(extract_section_value "Deletions:" "Total time:" "$RAW_LOG" || echo "")"
  DELETE_AVG="$(extract_section_value "Deletions:" "Avg time/op:" "$RAW_LOG" || echo "")"

  VALIDATION_CHECKS="$(extract_section_value "Validation:" "Checks:" "$RAW_LOG" || echo "")"
  VALIDATION_ERRORS="$(extract_section_value "Validation:" "Errors:" "$RAW_LOG" || echo "")"
  VALIDATION_TOTAL="$(extract_section_value "Validation:" "Total time:" "$RAW_LOG" || echo "")"
  VALIDATION_AVG="$(extract_section_value "Validation:" "Avg time/check:" "$RAW_LOG" || echo "")"

echo "${run},${order_in_run},${method},${TRACE_FILE},${VALIDATE_MODE},${TOTAL_LINES},${PROCESSED_OPS},${SKIPPED_LINES},${NUM_NODES},${INSERT_COUNT},${INSERT_TOTAL},${INSERT_AVG},${DELETE_COUNT},${DELETE_TOTAL},${DELETE_AVG},${VALIDATION_CHECKS},${VALIDATION_ERRORS},${VALIDATION_TOTAL},${VALIDATION_AVG},${EXIT_CODE},${RAW_LOG}" >> "$SUMMARY_CSV"

    if [ "$EXIT_CODE" -ne 0 ]; then
      echo "  Warning: ${method} run ${run} exited with code ${EXIT_CODE}. See ${RAW_LOG}"
    fi
  done
done

echo
echo "Done."
echo "Summary CSV:"
echo "  $SUMMARY_CSV"
echo
echo "Quick average summary:"
awk -F, '
NR > 1 {
  method=$3;
  insert_sum[method]+=$12;
  delete_sum[method]+=$15;
  count[method]+=1;
}
END {
  for (m in count) {
    printf "%s: avg_insert_us=%.6f avg_delete_us=%.6f runs=%d\n",
      m, insert_sum[m]/count[m], delete_sum[m]/count[m], count[m];
  }
}
' "$SUMMARY_CSV"
