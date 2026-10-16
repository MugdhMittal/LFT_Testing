#!/usr/bin/env bash
# Runs run_lft_benchmark.sh (build + sweep) on MAGUS as a 2-task Slurm array.
# Both tasks share one results directory and claim binaries from a common
# queue (heaviest first), so the sweep finishes as soon as the work is used up
# whichever node is faster, and a single task still completes everything if
# the second one never gets a node. The medium QOS allows 2 running jobs/user.
# Submit from microbench/ (path must contain no spaces):   sbatch run_lft_benchmark.sbatch
# Override at submit time, e.g.:
#   sbatch --export=ALL,THREADS="1 2 4 8" run_lft_benchmark.sbatch
# Build only:   sbatch --export=ALL,MODE=build --array=0 run_lft_benchmark.sbatch
# Results: results/lft_<arrayjobid>/results_w0.csv and results_w1.csv
#   merge:  cat results_w0.csv <(tail -n +2 results_w1.csv) > results.csv

#SBATCH --job-name=lft-bench
#SBATCH --partition=medium
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --array=0-1
#SBATCH --output=slurm-lft-%A_%a.out
#SBATCH --error=slurm-lft-%A_%a.err

set -euo pipefail

module purge
module load gromacs/2023.2
module load GCC/10.2.0

# Directory sbatch was invoked from (Slurm copies the script, so $0 is unreliable).
cd "${SLURM_SUBMIT_DIR:-$HOME/LFT/setbench/microbench}"

export WORKER="${SLURM_ARRAY_TASK_ID:-0}"
export RUN_ID="${RUN_ID:-lft_${SLURM_ARRAY_JOB_ID:-${SLURM_JOB_ID}}}"
export THREADS="${THREADS:-1 2 4 8 16 32 64 128}"

srun --cpu-bind=cores bash ./run_lft_benchmark.sh "${MODE:-all}"
