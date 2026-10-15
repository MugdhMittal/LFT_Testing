#!/bin/bash
set -uo pipefail

# ---------------------------------------------------------------------------
# Benchmark sweep for the augmented trees and tries (setbench).
#   Trees: Lock_Free_Tree_Augmented, Lock_Free_Tree_Augmented_FatNodePtrs
#   Tries: Trie_Baseline, Trie_FatNode_NoVC
# Step 1 rebuilds every binary (one per array size for the fat-node
# structures) into bin/lft/ using the Makefile's own settings; Step 2 runs
# the sweep with those binaries.
# Edit the structures / sizes / workloads below as needed.
#
# Usage (from microbench/):
#   bash run_lft_benchmark.sh                     # build, then run
#   bash run_lft_benchmark.sh build               # build only
#   THREADS="1 2 4 8" bash run_lft_benchmark.sh   # pick thread counts for this machine
#
# Thread counts can go up to 128 (max_threads in the Makefile).
#
# Output:
#   results/<RUN_ID>/results.csv   one averaged row per combination
#   results/<RUN_ID>/raw_logs/...  every trial's full log
# ---------------------------------------------------------------------------

cd "$(dirname "$0")"

# --- Structures to run (comment out any you don't want this run) ---
TREE_FIXED=(
    Lock_Free_Tree_Augmented
)
TREE_ARRAY=(
    Lock_Free_Tree_Augmented_FatNodePtrs
)
TRIE_FIXED=(
    Trie_Baseline
)
TRIE_ARRAY=(
    Trie_FatNode_NoVC
)

ARRAY_SIZES=(1 10 100 500)
TRIALS=2
# Override from the environment, e.g. THREADS="1 2 4 8 16"
# shellcheck disable=SC2206
THREADS=(${THREADS:-1 2 4 8 16 32 64 128})
DATA_SIZES=(1000 50000 200000)
WORKLOAD_PCTS=(50 25 1)

MAX_THREADS=128
RQ=0
RQSIZE=1
NRQ=0
TIME_MS=3000

BIN_DIR="bin/lft"
RUN_ID="${RUN_ID:-lft_$(date +%Y%m%d_%H%M%S)}"
RESULTS_DIR="results/${RUN_ID}"
CSV_PATH="${RESULTS_DIR}/results.csv"
MODE="${1:-all}"

# --- Preflight ---
for tool in make g++ awk grep; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool not found: $tool" >&2
        exit 1
    fi
done

for t in "${THREADS[@]}"; do
    if ! [[ "$t" =~ ^[0-9]+$ ]] || (( t < 1 || t > MAX_THREADS )); then
        echo "ERROR: thread count '$t' must be between 1 and ${MAX_THREADS}." >&2
        exit 1
    fi
done

# --- Binary list: "structure|array_size|binary" ---
JOBS=()
for ds in "${TREE_FIXED[@]}"; do JOBS+=("${ds}|NA|${BIN_DIR}/${ds}.debra"); done
for ds in "${TREE_ARRAY[@]}"; do
    for a in "${ARRAY_SIZES[@]}"; do JOBS+=("${ds}|${a}|${BIN_DIR}/${ds}_arr${a}.debra"); done
done
for ds in "${TRIE_FIXED[@]}"; do JOBS+=("${ds}|NA|${BIN_DIR}/${ds}.debra"); done
for ds in "${TRIE_ARRAY[@]}"; do
    for a in "${ARRAY_SIZES[@]}"; do JOBS+=("${ds}|${a}|${BIN_DIR}/${ds}_arr${a}.debra"); done
done

# ===========================================================================
# Step 1: rebuild every binary
# ===========================================================================
mkdir -p "$BIN_DIR"
build_total=${#JOBS[@]}
build_count=0
echo "=== Building ${build_total} binaries into ${BIN_DIR}/ ==="
for job in "${JOBS[@]}"; do
    IFS='|' read -r ds arr bin <<< "$job"
    build_count=$((build_count + 1))
    echo "[${build_count}/${build_total}] Building ${ds} arr=${arr}"
    array_flag=""
    [[ "$arr" != NA ]] && array_flag="-DFATNODE_ARRAY_SIZE=${arr}"
    if ! build_out=$(make -B "${ds}.debra" \
            DATA_STRUCTURES="$ds" \
            bin_dir="$BIN_DIR" \
            xargs="$array_flag" 2>&1); then
        echo "ERROR: build failed for ${ds} arr=${arr}:" >&2
        grep -m 20 -E "error|Error" <<< "$build_out" >&2
        exit 1
    fi
    if [[ "$arr" != NA ]]; then
        cp -f "${BIN_DIR}/${ds}.debra" "$bin"
    fi
done

if [[ "$MODE" == build ]]; then
    echo "Build complete."
    exit 0
fi

# ===========================================================================
# Step 2: run the sweep
# ===========================================================================
mkdir -p "${RESULTS_DIR}/raw_logs"

total=$(( ${#JOBS[@]} * ${#DATA_SIZES[@]} * ${#WORKLOAD_PCTS[@]} * ${#THREADS[@]} * TRIALS ))

# CSV header
trial_cols=""
for i in $(seq 1 "$TRIALS"); do trial_cols+=",throughput_trial${i}"; done
if [[ ! -f "$CSV_PATH" ]]; then
    echo "file_executed,array_size,workload,insert_pct,delete_pct,lookup_pct,threads,ds_size_k,time_ms,validation,throughput${trial_cols},find_throughput,update_throughput,total_ops,total_find,total_inserts,total_deletes,total_updates,versions_created" > "$CSV_PATH"
fi

echo
echo "=== Total runs: ${total} ==="
echo "Trees:       ${TREE_FIXED[*]} ${TREE_ARRAY[*]}"
echo "Tries:       ${TRIE_FIXED[*]} ${TRIE_ARRAY[*]}"
echo "Array sizes: ${ARRAY_SIZES[*]} (fat-node structures only)"
echo "DS sizes(k): ${DATA_SIZES[*]}"
echo "Workloads:   ${WORKLOAD_PCTS[*]} (% insert = % delete)"
echo "Threads:     ${THREADS[*]}"
echo "Trials:      ${TRIALS} x ${TIME_MS} ms"
echo "Results:     ${RESULTS_DIR}/"
echo

# --- Log parsing helpers ---
# Value after "key=" on the first matching line; empty if absent.
value_of() { grep -m1 "^$1=" "$2" | cut -d= -f2- | tr -d ' \r' || true; }
# GSTATS counter; gstats prints an empty value when the total is zero.
counter_of() {
    if grep -q "^$1=" "$2"; then
        local v; v=$(value_of "$1" "$2"); echo "${v:-0}"
    else
        echo "NA"
    fi
}
# Mean of the non-NA values in "$@"; NA if none.
mean_of() { printf '%s\n' "$@" | awk '$1!="NA" && $1!="" {s+=$1; n++} END {if (n>0) printf "%.2f", s/n; else print "NA"}'; }

run_count=0
completed=0

for job in "${JOBS[@]}"; do
    IFS='|' read -r DS_NAME ARRAY_SIZE BIN <<< "$job"

    LOG_ROOT="${RESULTS_DIR}/raw_logs/${DS_NAME}_arr${ARRAY_SIZE}"

    for k in "${DATA_SIZES[@]}"; do
        for pct in "${WORKLOAD_PCTS[@]}"; do
            for nthreads in "${THREADS[@]}"; do

                # --- Accumulate metrics across trials for this parameter combo ---
                declare -A M=()
                metrics=(throughput find_throughput update_throughput total_ops total_find total_inserts total_deletes total_updates versions)
                for m in "${metrics[@]}"; do M[$m]=""; done
                validated=0

                for trial in $(seq 1 "$TRIALS"); do
                    run_count=$((run_count + 1))
                    logdir="${LOG_ROOT}/k${k}/${pct}pct/t${nthreads}"
                    mkdir -p "$logdir"
                    logfile="${logdir}/trial${trial}.log"

                    printf '[%d/%d] %s arr=%s k=%s workload=%spct threads=%s trial=%s ... ' \
                        "$run_count" "$total" "$DS_NAME" "$ARRAY_SIZE" "$k" "$pct" "$nthreads" "$trial"

                    timeout --signal=KILL $((TIME_MS / 1000 + 900)) \
                        "$BIN" \
                        -nwork "$nthreads" \
                        -nprefill "$nthreads" \
                        -i "$pct" \
                        -d "$pct" \
                        -rq "$RQ" \
                        -rqsize "$RQSIZE" \
                        -k "$k" \
                        -nrq "$NRQ" \
                        -t "$TIME_MS" > "$logfile" 2>&1
                    exit_code=$?

                    # --- Parse metrics from log ---
                    throughput=$(value_of total_throughput "$logfile")
                    valid=no
                    grep -q '^Structural validation OK\.' "$logfile" && valid=yes
                    [[ $valid == yes ]] && validated=$((validated + 1))

                    M[throughput]+="${throughput:-NA} "
                    M[find_throughput]+="$(value_of find_throughput "$logfile") "
                    M[update_throughput]+="$(value_of update_throughput "$logfile") "
                    M[total_ops]+="$(value_of total_ops "$logfile") "
                    M[total_find]+="$(value_of total_find "$logfile") "
                    M[total_inserts]+="$(value_of total_inserts "$logfile") "
                    M[total_deletes]+="$(value_of total_deletes "$logfile") "
                    M[total_updates]+="$(value_of total_updates "$logfile") "
                    M[versions]+="$(counter_of sum_versions_created_total "$logfile") "

                    if [[ $exit_code -eq 0 && $valid == yes && -n "$throughput" ]]; then
                        completed=$((completed + 1))
                        printf 'OK throughput=%s\n' "$throughput"
                    else
                        printf 'FAILED (exit=%s, validation=%s) log=%s\n' "$exit_code" "$valid" "$logfile"
                    fi
                done

                # --- Average across trials (NA entries are ignored; NA if all trials NA) ---
                declare -A A=()
                for m in "${metrics[@]}"; do
                    # shellcheck disable=SC2086
                    A[$m]=$(mean_of ${M[$m]})
                done

                # shellcheck disable=SC2206
                tp=(${M[throughput]})
                trial_vals=""
                for v in "${tp[@]}"; do trial_vals+=",${v}"; done

                validation="FAIL"
                (( validated == TRIALS )) && validation="OK"

                lookup_pct=$((100 - 2 * pct))
                echo "${DS_NAME},${ARRAY_SIZE},${pct}pct,${pct},${pct},${lookup_pct},${nthreads},${k},${TIME_MS},${validation},${A[throughput]}${trial_vals},${A[find_throughput]},${A[update_throughput]},${A[total_ops]},${A[total_find]},${A[total_inserts]},${A[total_deletes]},${A[total_updates]},${A[versions]}" >> "$CSV_PATH"
                unset M A
            done
        done
    done
done

echo
echo "Done. ${completed}/${total} runs completed ($((total - completed)) failed)."
echo "Results: ${CSV_PATH}"
echo "Raw logs: ${RESULTS_DIR}/raw_logs/"
