#!/bin/bash
# Automated test and measurement run, started on the development host (the
# binaries must be deployed: make deploy). Runs every scenario N times on the
# target, fetches the raw data and prints a summary table with PASS/FAIL.
#
#   scripts/run-all.sh [runs] [outdir] [mode[:clients] ...]
#
# Defaults: 10 runs of every scenario, into results/latest. Each run takes
# about 15 s. The exit code is non-zero if any scenario fails.
#
# Environment: TARGET (ssh host, default qnxvm), REMOTE (binaries' directory,
# default /tmp/hotswap), SUDO_PW (target root password for sudo, default
# qnxuser, the quickstart image's).
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
N=${1:-10}
OUT=${2:-$HERE/results/latest}
[ $# -ge 1 ] && shift
[ $# -ge 1 ] && shift
if [ $# -eq 0 ]; then
    set -- none swap rollback late-crash hang kill rogue restart restart-mbb \
           swap:4 rollback:4
fi
TARGET=${TARGET:-qnxvm}
REMOTE=${REMOTE:-/tmp/hotswap}
SUDO_PW=${SUDO_PW:-qnxuser}
RDIR=/tmp/hs-series

# Every number below depends on the target's clock, so check it first: a
# wrong timer setting once made a VM's clock run five times too slow without
# any other symptom (see tools/patch_ifs_timer.py).
t0=$(date +%s%N)
ssh "$TARGET" 'sleep 5'
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
echo "target clock check: 'sleep 5' took $ms ms"
if [ "$ms" -lt 4800 ] || [ "$ms" -gt 6500 ]; then
    echo "target clock is wrong, not measuring" >&2
    exit 2
fi

echo "running $# scenario(s) x $N on $TARGET ..."
ssh "$TARGET" "rm -f /tmp/hs-series.log; cd $REMOTE; echo $SUDO_PW | sudo -S -p '' sh ./run-series.sh $N $RDIR $* > /tmp/hs-series.log 2>&1"

# Fetch everything through one connection (scp hangs on this image).
mkdir -p "$OUT"
rm -f "$OUT"/*.csv "$OUT"/*.log
ssh "$TARGET" "cd $RDIR; for f in *; do echo \"@@@FILE \$f\"; cat \"\$f\"; done" |
    awk -v dest="$OUT" '/^@@@FILE /{ if (out) close(out); out = dest "/" $2; next } { print > out }'

PY=python3
[ -x "$HERE/.venv/bin/python" ] && PY=$HERE/.venv/bin/python
"$PY" "$HERE/tools/summarize.py" "$OUT"
