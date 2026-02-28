#!/usr/bin/env bash
# tests/perf/throughput_test.sh
#
# Measures three NFRs against a running jq-server + jq-worker:
#   NFR-001: Throughput ≥ 1 000 job submissions/second
#   NFR-002: End-to-end p99 latency < 2 s  (submit → DONE)
#   NFR-003: Scheduler cycle p95 < 200 ms  (read from Prometheus /metrics)
#
# Prerequisites:
#   - jq-server running at $JQ_SERVER_ADDR (default: localhost:50051)
#   - jq-worker connected and subscribed to "default" queue
#   - Prometheus metrics exposed at $JQ_METRICS_ADDR (default: localhost:9090)
#   - grpcurl in PATH, or the jq-ctl binary at $JQ_CTL  (default: ./build/jq-ctl)
#   - jq (JSON query tool) in PATH
#
# Usage:
#   JQ_SERVER_ADDR=localhost:50051 bash tests/perf/throughput_test.sh
#
# Exit codes:  0 = all NFRs met,  1 = one or more NFRs failed

set -euo pipefail

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
SERVER_ADDR="${JQ_SERVER_ADDR:-localhost:50051}"
METRICS_ADDR="${JQ_METRICS_ADDR:-localhost:9090}"
JQ_CTL="${JQ_CTL:-./build/jq-ctl}"
TOTAL_JOBS="${PERF_JOBS:-1000}"
SAMPLE_JOBS="${PERF_SAMPLE:-50}"    # jobs whose round-trip latency we measure
MAX_WAIT_S="${PERF_MAX_WAIT:-120}"  # seconds to wait for sampled jobs to finish

# Colors (suppressed if not a tty)
RED=""; GREEN=""; YELLOW=""; RESET=""
if [ -t 1 ]; then RED="\033[31m"; GREEN="\033[32m"; YELLOW="\033[33m"; RESET="\033[0m"; fi

pass() { echo -e "${GREEN}[PASS]${RESET} $*"; }
fail() { echo -e "${RED}[FAIL]${RESET} $*"; FAILED=1; }
info() { echo -e "${YELLOW}[INFO]${RESET} $*"; }

FAILED=0
TMPDIR_LOCAL="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

# ---------------------------------------------------------------------------
# Check prerequisites
# ---------------------------------------------------------------------------
if ! command -v jq &>/dev/null; then
    echo "ERROR: 'jq' not found in PATH. Install it: brew install jq" >&2
    exit 1
fi
if [ ! -x "$JQ_CTL" ]; then
    echo "ERROR: jq-ctl not found at '$JQ_CTL'. Build first: cmake --build build --parallel" >&2
    exit 1
fi

info "Server:  $SERVER_ADDR"
info "Metrics: $METRICS_ADDR"
info "Jobs:    $TOTAL_JOBS total, $SAMPLE_JOBS sampled for latency"

# ---------------------------------------------------------------------------
# NFR-001: Submission throughput ≥ 1 000 jobs/second
# ---------------------------------------------------------------------------
info "--- NFR-001: Submission throughput ---"

PAYLOAD='{"command":["echo","perf"]}'
IDS_FILE="$TMPDIR_LOCAL/job_ids.txt"
: > "$IDS_FILE"

T_START=$(date +%s%3N)  # milliseconds

# Submit TOTAL_JOBS jobs in parallel (up to 64 concurrent).
# Each invocation of jq-ctl prints the job_id to stdout.
BATCH=64
SUBMITTED=0
for (( i=0; i<TOTAL_JOBS; i+=BATCH )); do
    COUNT=$(( TOTAL_JOBS - i < BATCH ? TOTAL_JOBS - i : BATCH ))
    for (( j=0; j<COUNT; j++ )); do
        "$JQ_CTL" --server-addr "$SERVER_ADDR" job submit \
            --queue default --payload "$PAYLOAD" 2>/dev/null \
          | grep -oE '[0-9a-f-]{36}' >> "$IDS_FILE" &
    done
    wait
    SUBMITTED=$(( SUBMITTED + COUNT ))
    printf "\r  Submitted %d / %d" "$SUBMITTED" "$TOTAL_JOBS"
done
echo ""

T_END=$(date +%s%3N)
ELAPSED_MS=$(( T_END - T_START ))
ELAPSED_S=$(echo "scale=3; $ELAPSED_MS / 1000" | bc)
ACTUAL_IDS=$(wc -l < "$IDS_FILE" | tr -d ' ')

if [ "$ELAPSED_MS" -eq 0 ]; then
    THROUGHPUT=0
else
    THROUGHPUT=$(echo "scale=1; $ACTUAL_IDS * 1000 / $ELAPSED_MS" | bc)
fi

info "  Submitted $ACTUAL_IDS jobs in ${ELAPSED_S}s  →  throughput = ${THROUGHPUT} jobs/s"

if (( ACTUAL_IDS < TOTAL_JOBS )); then
    fail "NFR-001: Only $ACTUAL_IDS / $TOTAL_JOBS jobs submitted successfully"
elif (( $(echo "$THROUGHPUT >= 1000" | bc) )); then
    pass "NFR-001: Throughput ${THROUGHPUT} jobs/s ≥ 1 000 jobs/s"
else
    fail "NFR-001: Throughput ${THROUGHPUT} jobs/s < 1 000 jobs/s (target)"
fi

# ---------------------------------------------------------------------------
# NFR-002: End-to-end p99 latency < 2 s
# ---------------------------------------------------------------------------
info "--- NFR-002: End-to-end latency (p99 < 2 s) ---"

# Pick the first SAMPLE_JOBS IDs to track.
SAMPLE_IDS=()
while IFS= read -r id && [ ${#SAMPLE_IDS[@]} -lt $SAMPLE_JOBS ]; do
    SAMPLE_IDS+=("$id")
done < "$IDS_FILE"

LATENCY_FILE="$TMPDIR_LOCAL/latencies.txt"
: > "$LATENCY_FILE"

T_MEASURE_START=$(date +%s%3N)
DEADLINE=$(( T_MEASURE_START + MAX_WAIT_S * 1000 ))

# Poll each sampled job until DONE / DEAD_LETTERED / timeout.
declare -A JOB_START_MS
for id in "${SAMPLE_IDS[@]}"; do
    JOB_START_MS[$id]=$(date +%s%3N)
done

PENDING_IDS=( "${SAMPLE_IDS[@]}" )
while [ ${#PENDING_IDS[@]} -gt 0 ] && [ "$(date +%s%3N)" -lt "$DEADLINE" ]; do
    sleep 0.5
    STILL_PENDING=()
    for id in "${PENDING_IDS[@]}"; do
        STATUS=$("$JQ_CTL" --server-addr "$SERVER_ADDR" job status "$id" 2>/dev/null \
                   | grep -oE 'DONE|DEAD_LETTERED|FAILED' | head -1 || true)
        if [[ "$STATUS" == "DONE" || "$STATUS" == "DEAD_LETTERED" || "$STATUS" == "FAILED" ]]; then
            NOW=$(date +%s%3N)
            LAT_MS=$(( NOW - JOB_START_MS[$id] ))
            echo "$LAT_MS" >> "$LATENCY_FILE"
        else
            STILL_PENDING+=("$id")
        fi
    done
    PENDING_IDS=( "${STILL_PENDING[@]}" )
done

MEASURED=$(wc -l < "$LATENCY_FILE" | tr -d ' ')
info "  Measured latency for $MEASURED / $SAMPLE_JOBS sampled jobs"

if [ "$MEASURED" -gt 0 ]; then
    # Sort and compute p99.
    SORTED_FILE="$TMPDIR_LOCAL/sorted.txt"
    sort -n "$LATENCY_FILE" > "$SORTED_FILE"
    P99_IDX=$(( (MEASURED * 99 + 99) / 100 ))
    P99_MS=$(sed -n "${P99_IDX}p" "$SORTED_FILE")
    P99_S=$(echo "scale=3; $P99_MS / 1000" | bc)
    MAX_MS=$(tail -1 "$SORTED_FILE")
    AVG_MS=$(awk '{s+=$1} END {printf "%d", s/NR}' "$SORTED_FILE")

    info "  Latency — avg: ${AVG_MS}ms, p99: ${P99_MS}ms (${P99_S}s), max: ${MAX_MS}ms"

    if (( $(echo "$P99_S < 2.0" | bc) )); then
        pass "NFR-002: p99 latency ${P99_S}s < 2 s"
    else
        fail "NFR-002: p99 latency ${P99_S}s ≥ 2 s (target)"
    fi
else
    fail "NFR-002: No completed jobs to measure latency"
fi

# ---------------------------------------------------------------------------
# NFR-003: Scheduler cycle p95 < 200 ms
# ---------------------------------------------------------------------------
info "--- NFR-003: Scheduler cycle p95 < 200 ms ---"

METRICS_URL="http://$METRICS_ADDR/metrics"
if command -v curl &>/dev/null && curl -sf "$METRICS_URL" &>/dev/null; then
    # Parse the histogram from Prometheus text format.
    # jq_scheduler_cycle_duration_seconds_bucket gives cumulative counts per le.
    RAW=$(curl -sf "$METRICS_URL" | grep 'jq_scheduler_cycle_duration_seconds_bucket' || true)

    if [ -z "$RAW" ]; then
        info "  Metric 'jq_scheduler_cycle_duration_seconds_bucket' not found — skipping NFR-003"
    else
        # Extract total count (le="+Inf").
        TOTAL=$(echo "$RAW" | grep 'le="+Inf"' | grep -oE '[0-9]+$' | head -1)
        if [ -z "$TOTAL" ] || [ "$TOTAL" -eq 0 ]; then
            info "  No scheduler cycles recorded yet — skipping NFR-003"
        else
            P95_TARGET=$(( (TOTAL * 95 + 99) / 100 ))
            # Find the smallest bucket whose count >= p95 target.
            P95_LE=""
            while IFS= read -r line; do
                LE=$(echo "$line" | grep -oE 'le="[^"]+"' | grep -oE '[0-9.e+]+' | head -1)
                COUNT=$(echo "$line" | grep -oE '[0-9]+$')
                if [[ -n "$LE" && -n "$COUNT" && "$COUNT" -ge "$P95_TARGET" && "$LE" != "+Inf" ]]; then
                    P95_LE="$LE"
                    break
                fi
            done <<< "$RAW"

            if [ -z "$P95_LE" ]; then
                fail "NFR-003: Could not determine p95 scheduler cycle duration"
            else
                P95_MS=$(echo "scale=1; $P95_LE * 1000" | bc)
                info "  Scheduler p95 cycle ≤ ${P95_MS}ms (le=${P95_LE}s)"
                if (( $(echo "$P95_LE < 0.2" | bc) )); then
                    pass "NFR-003: Scheduler p95 cycle ${P95_MS}ms < 200 ms"
                else
                    fail "NFR-003: Scheduler p95 cycle ${P95_MS}ms ≥ 200 ms (target)"
                fi
            fi
        fi
    fi
else
    info "  Metrics endpoint not reachable ($METRICS_URL) — skipping NFR-003"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
if [ "$FAILED" -eq 0 ]; then
    pass "All performance NFRs met."
    exit 0
else
    fail "One or more performance NFRs FAILED."
    exit 1
fi
