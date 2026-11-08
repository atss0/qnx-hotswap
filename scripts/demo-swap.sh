#!/bin/sh
# Live-swap demo, run ON the QNX target as root (attaching /dev/counter needs
# the pathspace ability):
#
#   cd /tmp/hotswap && ./demo-swap.sh [seconds] [swap|spawn|none]
#
# Timeline: shell + core v1 start, the 1 kHz client runs for <seconds>
# (default 10); at 3 s core v2 replaces v1, at 6 s v1 replaces v2 again
# (a downgrade is just another swap). The client must report
# errors=0 gaps=0 missed=0.
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
    slay -f core_v1 core_v2 2>/dev/null
}
trap cleanup EXIT INT TERM

event() {   # $1 = core to swap in
    case "$MODE" in
        swap)  echo "=== swapping in $1 ==="; "$DIR/$1" & ;;
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
event core_v2
sleep 3
event core_v1

wait "$CLIENT_PID"
RC=$?
echo "=== [$MODE] client exit code: $RC (0 = no errors, no gaps, no missed deadlines) ==="
exit $RC
