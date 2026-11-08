#!/bin/sh
# Live-swap demo, run ON the QNX target as root (attaching a /dev path needs
# the pathspace ability):
#
#   cd /tmp/hotswap && [DRV=counter|accel] ./demo-swap.sh [seconds] [mode]
#
# DRV picks the driver: counter (/dev/counter, default) or accel
# (/dev/accel, the simulated accelerometer). Every mode uses the same
# timeline: the driver starts, the 1 kHz client runs for <seconds>
# (default 10), and an upgrade happens at 3 s and at 6 s.
#
#   swap     - hot swap: v2 replaces v1 at 3 s (state format 1 -> 2,
#              converted), v1 replaces v2 at 6 s (a downgrade: v2 exports
#              format 1). Expect no failed reads and no state loss.
#   rollback - at 3 s a buggy v2 (<drv>_v2_bad) is swapped in and crashes on
#              its first read; the shell rolls back to v1 within the same
#              client request. At 6 s the good v2 is swapped in.
#   restart  - the conventional way, for comparison: the same driver code
#              built as a single-process resource manager (<drv>_mono_v1) is
#              stopped and <drv>_mono_v2 started at 3 s, and back at 6 s.
#              This is also what a QNX HAM restart action does.
#   spawn    - control: hot-swap driver, but at 3 s and 6 s only an
#              unrelated program (hello) is started
#   none     - control: nothing happens
#
# INIT_MS=<ms> gives every driver start (cores and mono alike) a simulated
# hardware initialisation time, e.g. INIT_MS=200 ./demo-swap.sh 10 restart
# SAMPLES=<file.csv> writes one line per client tick (see tools/plot.py).
set -u
DIR=$(dirname "$0")
SECS=${1:-10}
MODE=${2:-swap}
DRV=${DRV:-counter}
DEV=/dev/$DRV
INIT="-i ${INIT_MS:-0}"
DRV_PID=

cleanup() {
    [ -n "$DRV_PID" ] && kill "$DRV_PID" 2>/dev/null
    slay -f shell ${DRV}_v1 ${DRV}_v2 ${DRV}_v2_bad ${DRV}_mono_v1 ${DRV}_mono_v2 2>/dev/null
}
trap cleanup EXIT INT TERM

# Start the driver: shell + core v1, or the conventional mono v1.
if [ "$MODE" = restart ]; then
    "$DIR/${DRV}_mono_v1" $INIT &
    DRV_PID=$!
else
    "$DIR/shell" -n "$DEV" &
    DRV_PID=$!
    sleep 1
    "$DIR/${DRV}_v1" $INIT &
fi
sleep 2     # leave time for initialisation (and calibration, for accel)

# One upgrade step. $1 = core for swap mode, $2 = core for rollback mode,
# $3 = conventional driver for restart mode.
event() {
    case "$MODE" in
        swap)     echo "=== hot swap: starting $1 ==="; "$DIR/$1" $INIT & ;;
        rollback) echo "=== hot swap: starting $2 ==="; "$DIR/$2" $INIT & ;;
        restart)  echo "=== restart: stopping pid $DRV_PID, starting $3 ==="
                  kill "$DRV_PID"
                  "$DIR/$3" $INIT &
                  DRV_PID=$! ;;
        spawn)    echo "=== spawning hello (control) ==="; "$DIR/hello" >/dev/null & ;;
        none)     echo "=== no event (baseline) ===" ;;
    esac
}

"$DIR/${DRV}_client" "$SECS" 1000 50 ${SAMPLES:-} &
CLIENT_PID=$!

sleep 3
event ${DRV}_v2 ${DRV}_v2_bad ${DRV}_mono_v2
sleep 3
event ${DRV}_v1 ${DRV}_v2 ${DRV}_mono_v1

wait "$CLIENT_PID"
RC=$?
echo "=== [$DRV $MODE] client exit code: $RC (0 = no failures, no state loss, no missed deadlines) ==="
exit $RC
