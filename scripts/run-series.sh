#!/bin/sh
# Run demo scenarios several times, ON the QNX target, as root. Normally
# started by scripts/run-all.sh on the development host.
#
#   ./run-series.sh <runs> <outdir> <mode[:clients]>...
#
# For every run it leaves <name>-<i>.csv (per-tick samples) and <name>-<i>.log
# (the demo's output) in <outdir>; with several clients also
# <name>-<i>-c<n>.csv. One shell session runs the whole series so no SSH
# connection is set up while something is being measured.
set -u
N=$1
OUT=$2
shift 2
DIR=$(dirname "$0")
mkdir -p "$OUT"
rm -f "$OUT"/*

for spec in "$@"; do
    mode=${spec%%:*}
    clients=1
    case "$spec" in *:*) clients=${spec##*:} ;; esac
    name=$mode
    [ "$clients" != 1 ] && name="$mode-${clients}c"
    i=1
    while [ "$i" -le "$N" ]; do
        DRV=accel INIT_MS=200 CLIENTS=$clients SAMPLES="$OUT/$name-$i.csv" \
            "$DIR/demo-swap.sh" 10 "$mode" > "$OUT/$name-$i.log" 2>&1
        sleep 1
        i=$((i + 1))
    done
done
chmod a+r "$OUT"/*
echo "series done"
