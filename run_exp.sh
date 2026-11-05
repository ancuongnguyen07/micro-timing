#!/bin/bash

set -euo pipefail

PROG=$(basename "$0")
NUM_ITER=1
LATENCY_ONLY=0
TIME_SERIES=0
NOW=$(date +"%Y%m%d_%H%M%S")
LOG_DIR=logs
SIB_CMD=

print_help() {
    cat <<EOF
Usage: $PROG [-l] [-t] [-i num_runs] [-c command] [-h]

Build the micro-benchmark and run it num_runs times (sudo is used only when the PMC is read; -l and -t run without it).

Options:
  -l           Latency mode: measure with RDPRU only, do not open the PMC
  -t           Time series: probe latency over a window while the sibling
               alternates imul / nop segments (RDPRU only); writes
               odata/ts/*_timeseries.csv. Not combinable with -c.
  -i num_runs  Number of times to run the benchmark; positive integer
               (default: $NUM_ITER)
  -c command   Run this shell command on the SMT sibling during the busy sweep
               instead of the built-in load. It must keep running (e.g. a
               loop); stdout/stderr are discarded. It runs as root when sudo is used (no -l/-t),
               otherwise as the current user.
  -h           Show this help and exit

Examples:
  $PROG          # one run
  $PROG -i 5     # five runs
  $PROG -c 'while :; do :; done'   # busy sibling runs an endless empty loop
EOF
}

die() {
    echo "$PROG: $*" >&2
    echo "Try '$PROG -h' for more information." >&2
    exit 1
}

# Leading ':' = silent mode, so we report errors ourselves with the right text.
while getopts ":lti:c:h" opt; do
    case $opt in
        l)
            LATENCY_ONLY=1
            ;;
        t)
            TIME_SERIES=1
            ;;
        i)
            [[ $OPTARG =~ ^[1-9][0-9]*$ ]] \
                || die "-i expects a positive integer, got '$OPTARG'"
            NUM_ITER=$OPTARG
            ;;
        c)
            SIB_CMD=$OPTARG
            ;;
        h)
            print_help
            exit 0
            ;;
        :)
            die "option -$OPTARG requires an argument"
            ;;
        \?)
            die "invalid option: -$OPTARG"
            ;;
    esac
done
shift $((OPTIND - 1))

[[ $# -eq 0 ]] || die "unexpected argument: '$1'"
((TIME_SERIES && ${#SIB_CMD})) && die "-t and -c cannot be combined"

make
mkdir -p "$LOG_DIR"

for ((i = 1; i <= NUM_ITER; i++)); do
    echo "Running iteration $i of $NUM_ITER..."
    echo "=================================================="
    args=()
    ((LATENCY_ONLY)) && args+=(-l)
    ((TIME_SERIES)) && args+=(-t)
    [[ -n $SIB_CMD ]] && args+=(-c "$SIB_CMD")
    # RDPRU needs no privileges; only the PMC (perf_event_open) may need root
    runner=()
    ((LATENCY_ONLY || TIME_SERIES)) || runner=(sudo)
    "${runner[@]}" ./build/bench "${args[@]}" 1>"$LOG_DIR/${NOW}"_run${i}.log
    ((TIME_SERIES)) || python pysrc/csv_checker.py
done

echo "All $NUM_ITER iterations completed successfully."
