#!/bin/bash
set -uo pipefail

# ---------------------------------------------------------------------------
# Benchmark sweep for the augmented trees and tries (setbench).
#   Trees: Lock_Free_Tree_Augmented, Lock_Free_Tree_Augmented_FatNodePtrs
#   Tries: Trie_Baseline, Trie_FatNode_NoVC
# Step 1 rebuilds every binary (one per array size for the fat-node
# structures) into bin/ using the Makefile's own settings; Step 2 runs
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
    Lock_Free_Tree_Augmented_FatNodePtrs_Pad
)
TRIE_FIXED=(
    Trie_Baseline
)
TRIE_ARRAY=(
    Trie_FatNode_NoVC
    Trie_FatNode_NoVC_Pad
)

ARRAY_SIZES=(1 5 10 50 100 500 1000 2000)
TRIALS=3
# Override from the environment, e.g. THREADS="1 2 4 8 16"
# shellcheck disable=SC2206
THREADS=(${THREADS:-1 2 4 8 16 32 64 128})
DATA_SIZES=(1000 10000 20000 50000 100000 500000 1000000)
WORKLOAD_PCTS=(50 33 25 1)

MAX_THREADS=128
RQ=0
RQSIZE=1
NRQ=0
TIME_MS=3000

BIN_DIR="bin"
# Allocator preloaded into every benchmark run (not the build). Default is the
# bundled jemalloc; ALLOC_LIB="" runs with the system allocator.
ALLOC_LIB="${ALLOC_LIB-$(cd .. && pwd)/lib/libjemalloc.so}"
ALLOC_NAME="${ALLOC_LIB:+$(basename "$ALLOC_LIB")}"
ALLOC_NAME="${ALLOC_NAME:-system}"
RUN_ID="${RUN_ID:-lft_$(date +%Y%m%d_%H%M%S)}"
RESULTS_DIR="results/${RUN_ID}"
# Several workers (Slurm array tasks, possibly on different nodes) can share one
# RUN_ID: they claim binaries through ${RESULTS_DIR}/claims and each writes its
# own results_w<WORKER>.csv.
WORKER="${WORKER:-}"
CSV_PATH="${RESULTS_DIR}/results${WORKER:+_w${WORKER}}.csv"
MODE="${1:-all}"

# Pin worker thread i to logical CPU i (0..nproc-1). Set PIN=0 to leave
# threads to the OS scheduler.
PIN_ARGS=()
if [[ "${PIN-1}" != 0 ]]; then
    PIN_ARGS=(-pin "0-$(( $(nproc) - 1 ))")
fi

# --- Preflight ---
for tool in make g++ awk grep; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool not found: $tool" >&2
        exit 1
    fi
done

# LD_PRELOAD splits on spaces and colons, and ld.so only warns (exit status 0)
# when it cannot load a library, so check the path and the warning explicitly.
if [[ -n "$ALLOC_LIB" ]]; then
    if [[ "$ALLOC_LIB" == *[[:space:]:]* ]]; then
        echo "ERROR: allocator path must not contain spaces or ':' : $ALLOC_LIB" >&2
        echo "       Set ALLOC_LIB to a copy of the library in a plain path." >&2
        exit 1
    fi
    if [[ -n "$(LD_PRELOAD="$ALLOC_LIB" true 2>&1)" ]]; then
        echo "ERROR: cannot preload allocator: $ALLOC_LIB" >&2
        exit 1
    fi
fi

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

# Heaviest binaries first (rough cost: array size x 2 if padded x 4 for tries,
# whose node count is ~4x a tree's), so workers finish at about the same time.
job_weight() {
    local ds=$1 arr=$2 w
    [[ "$arr" == NA ]] && { echo 1; return; }
    w=$arr
    [[ "$ds" == *_Pad ]] && w=$((w * 2))
    [[ "$ds" == Trie_* ]] && w=$((w * 4))
    echo "$w"
}
mapfile -t JOBS < <(
    for job in "${JOBS[@]}"; do
        IFS='|' read -r d a b <<< "$job"
        echo "$(job_weight "$d" "$a")|${job}"
    done | sort -t'|' -k1,1nr -s | cut -d'|' -f2-
)

# Build one binary into a private temp dir, then move it into bin/ (two
# workers can build different array sizes of one structure at the same time).
build_job() {
    local ds=$1 arr=$2 bin=$3 array_flag="" tmp build_out
    [[ "$arr" != NA ]] && array_flag="-DFATNODE_ARRAY_SIZE=${arr}"
    tmp=$(mktemp -d "${BIN_DIR}/.build.XXXXXX")
    if ! build_out=$(make -B "${ds}.debra" \
            DATA_STRUCTURES="$ds" \
            bin_dir="$tmp" \
            xargs="$array_flag" 2>&1); then
        echo "ERROR: build failed for ${ds} arr=${arr}:" >&2
        grep -m 20 -E "error|Error" <<< "$build_out" >&2
        rm -rf "$tmp"
        return 1
    fi
    mv -f "${tmp}/${ds}.debra" "$bin"
    rm -rf "$tmp"
}

mkdir -p "$BIN_DIR"
if [[ "$MODE" == build ]]; then
    echo "=== Building ${#JOBS[@]} binaries into ${BIN_DIR}/ ==="
    n=0
    for job in "${JOBS[@]}"; do
        IFS='|' read -r ds arr bin <<< "$job"
        n=$((n + 1))
        echo "[${n}/${#JOBS[@]}] Building ${ds} arr=${arr}"
        build_job "$ds" "$arr" "$bin" || exit 1
    done
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
    echo "file_executed,array_size,workload,insert_pct,delete_pct,lookup_pct,threads,ds_size_k,time_ms,validation,throughput${trial_cols},find_throughput,update_throughput,total_ops,total_find,total_inserts,total_deletes,total_updates,versions_created,struct_sizes_bytes,allocator" > "$CSV_PATH"
fi

echo
echo "=== Total runs in sweep: ${total} (this worker takes whichever binaries it claims) ==="
echo "Trees:       ${TREE_FIXED[*]} ${TREE_ARRAY[*]}"
echo "Tries:       ${TRIE_FIXED[*]} ${TRIE_ARRAY[*]}"
echo "Array sizes: ${ARRAY_SIZES[*]} (fat-node structures only)"
echo "DS sizes(k): ${DATA_SIZES[*]}"
echo "Workloads:   ${WORKLOAD_PCTS[*]} (% insert = % delete)"
echo "Threads:     ${THREADS[*]}"
echo "Trials:      ${TRIALS} x ${TIME_MS} ms"
echo "Allocator:   ${ALLOC_NAME} (LD_PRELOAD)"
echo "Worker:      ${WORKER:-single}"
echo "Pinning:     ${PIN_ARGS[*]:-off}"
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

    # Claim this binary (atomic mkdir); skip it if another worker has it.
    mkdir -p "${RESULTS_DIR}/claims"
    mkdir "${RESULTS_DIR}/claims/${DS_NAME}_arr${ARRAY_SIZE}" 2>/dev/null || continue
    echo "=== Building ${DS_NAME} arr=${ARRAY_SIZE} ==="
    build_job "$DS_NAME" "$ARRAY_SIZE" "$BIN" || exit 1

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

                    printf '[%d] %s arr=%s k=%s workload=%spct threads=%s trial=%s ... ' \
                        "$run_count" "$DS_NAME" "$ARRAY_SIZE" "$k" "$pct" "$nthreads" "$trial"

                    ${RUN_TIMEOUT:+timeout --signal=KILL $RUN_TIMEOUT} \
                        env ${ALLOC_LIB:+LD_PRELOAD="$ALLOC_LIB"} "$BIN" \
                        "${PIN_ARGS[@]}" \
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
                    tk=$(value_of threads_final_keysum "$logfile")
                    dk=$(grep -m1 '^ds_final_keysum' "$logfile" | sed 's/^ds_final_keysum=\{0,1\}//' | tr -d ' \r')
                    [[ -n "$tk" && "$tk" == "$dk" ]] || valid=no
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
                    struct_sizes=$(value_of struct_sizes "$logfile")

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
                echo "${DS_NAME},${ARRAY_SIZE},${pct}pct,${pct},${pct},${lookup_pct},${nthreads},${k},${TIME_MS},${validation},${A[throughput]}${trial_vals},${A[find_throughput]},${A[update_throughput]},${A[total_ops]},${A[total_find]},${A[total_inserts]},${A[total_deletes]},${A[total_updates]},${A[versions]},${struct_sizes:-NA},${ALLOC_NAME}" >> "$CSV_PATH"
                unset M A
            done
        done
    done
done

echo
echo "Done. ${completed}/${run_count} runs completed by this worker ($((run_count - completed)) failed)."
echo "Results: ${CSV_PATH}"
echo "Raw logs: ${RESULTS_DIR}/raw_logs/"
