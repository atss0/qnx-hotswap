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
#   late-crash - like rollback, but the bad v2 crashes on its 500th read.
#              Probation checkpoints let v1 continue from read 499.
#   hang     - at 3 s a v2 that spins forever on its first read is swapped in;
#              the shell's timeout detects it and rolls back.
#   kill     - v2 is swapped in at 3 s and committed; at 6 s it is killed with
#              SIGKILL (no predecessor left). Reads return EAGAIN until a
#              supervisor (here: this script, 1 s later) starts a new core,
#              which continues from the last checkpoint.
#   rogue    - at 3 s an unrelated process tries to command the core.
#              Every attempt must be refused, the client must not notice.
#   restart  - the conventional way, for comparison: the same driver code
#              built as a single-process resource manager (<drv>_mono_v1) is
#              stopped and <drv>_mono_v2 started at 3 s, and back at 6 s.
#              Equivalent to a restart by a process supervisor.
#   restart-mbb - the best conventional upgrade: "make before break". The new
#              process initialises while the old one serves, reuses a saved
#              calibration and attaches in front of the old path, then the
#              old process is stopped.
#   spawn    - control: hot-swap driver, but at 3 s and 6 s only an
#              unrelated program (hello) is started
#   none     - control: nothing happens
#
# INIT_MS=<ms> gives every driver start (cores and mono alike) a simulated
# hardware initialisation time, e.g. INIT_MS=200 ./demo-swap.sh 10 restart
# SAMPLES=<file.csv> writes one line per client tick (see tools/plot.py).
# CLIENTS=<n> runs n clients at once on the same device (default 1); only
# each writes its own SAMPLES file (name-c<n>.csv for n > 1). The exit code
# is non-zero if any client saw a failure.
set -u
DIR=$(dirname "$0")
SECS=${1:-10}
MODE=${2:-swap}
DRV=${DRV:-counter}
# DEV=<path> overrides the device path, so two demos can run side by side
# (tools/live.py runs hot swap on /dev/accel and restart on /dev/accel_restart).
DEV=${DEV:-/dev/$DRV}
export HOTSWAP_DEV="$DEV"
INIT="-i ${INIT_MS:-0} -n $DEV"
DRV_PID=
CORE_PIDS=

# Stop only what this run started (another demo may be running in parallel).
cleanup() {
    for pid in $DRV_PID $CORE_PIDS; do
        kill "$pid" 2>/dev/null
    done
}
trap cleanup EXIT INT TERM

STATE_FILE=/tmp/hotswap-state-$$
rm -f "$STATE_FILE"
MBB="-b -s $STATE_FILE"

# Start the driver: shell + core v1, or the conventional mono v1.
case "$MODE" in
    restart)     "$DIR/${DRV}_mono_v1" $INIT & DRV_PID=$! ;;
    restart-mbb) "$DIR/${DRV}_mono_v1" $INIT $MBB & DRV_PID=$! ;;
    *)           "$DIR/shell" -n "$DEV" ${SHELL_OPTS:-} &
                 DRV_PID=$!
                 sleep 1
                 "$DIR/${DRV}_v1" $INIT &
                 LAST_CORE=$!
                 CORE_PIDS="$CORE_PIDS $LAST_CORE" ;;
esac
sleep 2     # leave time for initialisation (and calibration, for accel)

# Start a new core; the swap happens when it registers.
start_core() {
    echo "=== hot swap: starting $1 ==="
    "$DIR/$1" $INIT &
    LAST_CORE=$!
    CORE_PIDS="$CORE_PIDS $LAST_CORE"
}

# One upgrade step. $1 = step number (1 or 2).
event() {
    case "$MODE" in
        swap)       if [ "$1" = 1 ]; then start_core ${DRV}_v2; else start_core ${DRV}_v1; fi ;;
        rollback)   if [ "$1" = 1 ]; then start_core ${DRV}_v2_bad; else start_core ${DRV}_v2; fi ;;
        late-crash) if [ "$1" = 1 ]; then start_core ${DRV}_v2_late; else start_core ${DRV}_v2; fi ;;
        hang)       if [ "$1" = 1 ]; then start_core ${DRV}_v2_hang; else start_core ${DRV}_v2; fi ;;
        kill)       if [ "$1" = 1 ]; then start_core ${DRV}_v2; else
                        echo "=== kill -9 core (pid $LAST_CORE) ==="
                        kill -9 "$LAST_CORE"
                        sleep 1
                        start_core ${DRV}_v2
                    fi ;;
        rogue)      if [ "$1" = 1 ]; then
                        echo "=== rogue process attacks core (pid $LAST_CORE) ==="
                        "$DIR/rogue" "$LAST_CORE" || ROGUE_RC=1
                    else start_core ${DRV}_v2; fi ;;
        restart)    v=2; [ "$1" = 2 ] && v=1
                    echo "=== restart: stopping pid $DRV_PID, starting ${DRV}_mono_v$v ==="
                    kill "$DRV_PID"
                    "$DIR/${DRV}_mono_v$v" $INIT &
                    DRV_PID=$! ;;
        restart-mbb) v=2; [ "$1" = 2 ] && v=1
                    echo "=== make-before-break: starting ${DRV}_mono_v$v, then stopping pid $DRV_PID ==="
                    OLD=$DRV_PID
                    "$DIR/${DRV}_mono_v$v" $INIT $MBB &
                    DRV_PID=$!
                    sleep 1     # initialise and attach while the old one serves
                    kill "$OLD" ;;
        spawn)      echo "=== spawning hello (control) ==="; "$DIR/hello" >/dev/null & ;;
        none)       echo "=== no event (baseline) ===" ;;
    esac
}

ROGUE_RC=0
NCLIENTS=${CLIENTS:-1}
if [ "$NCLIENTS" -gt 1 ]; then
    export HOTSWAP_SHARED=1     # clients see interleaved sequence numbers
fi
CLIENT_PIDS=
n=1
while [ "$n" -le "$NCLIENTS" ]; do
    if [ "$n" -eq 1 ] || [ -z "${SAMPLES:-}" ]; then
        OUT=${SAMPLES:-}
    else
        OUT=${SAMPLES%.csv}-c$n.csv
    fi
    "$DIR/${DRV}_client" "$SECS" 1000 50 $OUT &
    CLIENT_PIDS="$CLIENT_PIDS $!"
    n=$((n + 1))
done

sleep 3
event 1
sleep 3
event 2

RC=0
for pid in $CLIENT_PIDS; do
    wait "$pid" || RC=1
done
[ "$ROGUE_RC" = 0 ] || RC=1
rm -f "$STATE_FILE"
echo "=== [$DRV $MODE, $NCLIENTS client(s)] exit code: $RC (0 = no failures, no state loss, no missed deadlines) ==="
exit $RC
