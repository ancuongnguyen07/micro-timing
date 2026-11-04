#!/bin/bash

set -euo pipefail

PROG=$(basename "$0")
NUM_ITER=1
SIB_CMD=

print_help() {
    cat <<EOF
Usage: $PROG [-i num_runs] [-c command] [-h]

Build the micro-benchmark and run it num_runs times (needs sudo for RDPRU/PMC).

Options:
  -i num_runs  Number of times to run the benchmark; positive integer
               (default: $NUM_ITER)
  -c command   Run this shell command on the SMT sibling during the busy sweep
               instead of the built-in load. It must keep running (e.g. a
               loop); stdout/stderr are discarded. It runs as root (sudo).
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
while getopts ":i:c:h" opt; do
    case $opt in
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

make

for ((i = 1; i <= NUM_ITER; i++)); do
    echo "Running iteration $i of $NUM_ITER..."
    echo "=================================================="
    if [[ -n $SIB_CMD ]]; then
        sudo ./build/bench -c "$SIB_CMD"
    else
        sudo ./build/bench
    fi
    python pysrc/csv_checker.py
done

echo "All $NUM_ITER iterations completed successfully."
