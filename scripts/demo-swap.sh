#!/bin/sh
# Live-swap demo, run ON the QNX target as root (attaching /dev/counter needs
# the pathspace ability):
#
#   cd /tmp/hotswap && ./demo-swap.sh [seconds]
#
# Timeline: shell + core v1 start, the 1 kHz client runs for <seconds>
# (default 10); at 3 s core v2 replaces v1, at 6 s v1 replaces v2 again
# (a downgrade is just another swap). The client must report
# errors=0 gaps=0 missed=0.
set -u
DIR=$(dirname "$0")
SECS=${1:-10}

cleanup() {
    kill "$SHELL_PID" 2>/dev/null
    slay -f core_v1 core_v2 2>/dev/null
}
trap cleanup EXIT INT TERM

"$DIR/shell" &
SHELL_PID=$!
sleep 1
"$DIR/core_v1" &
sleep 1

"$DIR/client" "$SECS" 1000 50 &
CLIENT_PID=$!

sleep 3
echo "=== swapping in core v2 ==="
"$DIR/core_v2" &
sleep 3
echo "=== swapping back to core v1 ==="
"$DIR/core_v1" &

wait "$CLIENT_PID"
RC=$?
echo "=== client exit code: $RC (0 = no errors, no gaps, no missed deadlines) ==="
exit $RC
