#!/bin/sh
# Live-swap demo, run ON the QNX target as root (attaching /dev/counter needs
# the pathspace ability):
#
#   cd /tmp/hotswap && ./demo-swap.sh [seconds] [swap|rollback|spawn|none]
#
# Timeline: shell + core v1 start, the 1 kHz client runs for <seconds>
# (default 10); at 3 s core v2 replaces v1 (state format 1 -> 2, converted),
# at 6 s v1 replaces v2 again (a downgrade: v2 exports format 1). The client
# must report errors=0 gaps=0 missed=0.
#
#   rollback - at 3 s a buggy v2 (core_v2_bad) is swapped in and crashes on
#              its first read; the shell rolls back to v1 within the same
#              client request. At 6 s the good v2 is swapped in.
#
# Control modes, same timeline, no swap:
#   spawn - start an unrelated program (hello) at 3 s and 6 s, to separate
#           the cost of process creation from the cost of the swap itself
#   none  - nothing happens; the plain baseline
set -u
DIR=$(dirname "$0")
SECS=${1:-10}
MODE=${2:-swap}

cleanup() {
    kill "$SHELL_PID" 2>/dev/null
    slay -f core_v1 core_v2 core_v2_bad 2>/dev/null
}
trap cleanup EXIT INT TERM

event() {   # $1 = core to swap in (swap mode), $2 = core for rollback mode
    case "$MODE" in
        swap)  echo "=== swapping in $1 ==="; "$DIR/$1" & ;;
        rollback) echo "=== swapping in $2 ==="; "$DIR/$2" & ;;
        spawn) echo "=== spawning hello (control) ==="; "$DIR/hello" >/dev/null & ;;
        none)  echo "=== no event (baseline) ===" ;;
    esac
}

"$DIR/shell" &
SHELL_PID=$!
sleep 1
"$DIR/core_v1" &
sleep 1

"$DIR/client" "$SECS" 1000 50 &
CLIENT_PID=$!

sleep 3
event core_v2 core_v2_bad
sleep 3
event core_v1 core_v2

wait "$CLIENT_PID"
RC=$?
echo "=== [$MODE] client exit code: $RC (0 = no errors, no gaps, no missed deadlines) ==="
exit $RC
