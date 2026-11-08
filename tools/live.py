#!/usr/bin/env python3
"""Live side-by-side view: hot swap vs. stop-and-restart, as it happens.

Starts two demo runs on the QNX target at the same time over SSH:
  - hot swap  on /dev/accel          (scripts/demo-swap.sh ... swap)
  - restart   on /dev/accel_restart  (scripts/demo-swap.sh ... restart)
Each run's 1 kHz client prints a summary line every 20 ms
(HOTSWAP_STREAM=20, see src/accel_client.c); this script plots them live:
  - top:    slowest read in each 20 ms window; red bands where reads failed
  - bottom: largest measurement error in each window
  - title:  running totals of failed reads and sequence breaks
Upgrade moments reported by the demo script are marked with a vertical line.

Needs Python with matplotlib and Tk (works on Windows), and SSH access to
the target with the binaries deployed (`make deploy`). The demo needs root
on the target; the sudo password is read from QNX_SUDO_PASSWORD
(default "qnxuser", the QEMU quickstart image's default).

Usage:
  python tools/live.py [--seconds 12] [--init-ms 200] [--save out.png]
  python tools/live.py --ssh "ssh -p 2223 -c aes128-ctr qnxuser@127.0.0.1"
"""
import argparse
import os
import shlex
import subprocess
import threading

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

VERSION_COLORS = {1: "#2f6db5", 2: "#e08a1e"}
FAIL_COLOR = "#d62728"
STREAM_TICKS = 20          # client summary every 20 ticks = 20 ms at 1 kHz

# Windows' OpenSSH client fails against this guest with its default cipher
# and MAC ("Corrupted MAC on input"); aes128-ctr + hmac-sha2-256 works.
DEFAULT_SSH = (
    "ssh -p 2223 -c aes128-ctr -m hmac-sha2-256 -o BatchMode=yes -o LogLevel=ERROR "
    "-o StrictHostKeyChecking=no "
    f"-o UserKnownHostsFile={os.devnull} "
    f"-i {os.path.expanduser('~/.ssh/qnxvm_ed25519')} qnxuser@127.0.0.1"
)


class Run:
    """One demo run on the target and the data received from it so far."""

    def __init__(self, title, mode, dev):
        self.title, self.mode, self.dev = title, mode, dev
        self.lock = threading.Lock()
        self.buckets = []      # (t_s, ok, failed, max_read_us, max_err, version, breaks)
        self.events = []       # (t_s, label)
        self.log = []          # other output lines, for the console
        self.done = False

    def start(self, ssh, seconds, init_ms, password, remote_dir):
        remote = (
            f"cd {remote_dir} && echo {shlex.quote(password)} | sudo -S -p '' "
            f"env DRV=accel DEV={self.dev} INIT_MS={init_ms} HOTSWAP_STREAM={STREAM_TICKS} "
            f"./demo-swap.sh {seconds} {self.mode} 2>&1"
        )
        self.proc = subprocess.Popen(
            shlex.split(ssh, posix=(os.name != "nt")) + [remote],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
        )
        threading.Thread(target=self._reader, daemon=True).start()

    def _last_t(self):
        return self.buckets[-1][0] if self.buckets else 0.0

    def _reader(self):
        for line in self.proc.stdout:
            line = line.rstrip()
            parts = line.split()
            with self.lock:
                if parts and parts[0] == "S" and len(parts) == 8:
                    t_ms, ok, fail, rd, err, ver, brk = parts[1:]
                    self.buckets.append((float(t_ms) / 1000.0, int(ok), int(fail),
                                         float(rd), int(err), int(ver), int(brk)))
                    continue
                if line.startswith("=== hot swap") or line.startswith("=== restart"):
                    self.events.append((self._last_t(), "upgrade"))
                elif "ROLLBACK" in line:
                    self.events.append((self._last_t(), "rollback"))
                if not line.startswith("t="):
                    self.log.append(line)
                    print(f"[{self.mode}] {line}", flush=True)
        self.proc.wait()
        with self.lock:
            self.done = True


def draw_run(ax_top, ax_bot, run, seconds):
    with run.lock:
        b = list(run.buckets)
        events = list(run.events)
        done = run.done
    ax_top.clear()
    ax_bot.clear()

    failed = sum(x[2] for x in b)
    breaks = sum(x[6] for x in b)
    version = b[-1][5] if b else 0
    status = "finished" if done else "running"
    ax_top.set_title(f"{run.title}\nfailed reads: {failed}   sequence breaks: {breaks}   "
                     f"driver: v{version}   ({status})", fontsize=10)

    for ver, color in VERSION_COLORS.items():
        xs = [x[0] for x in b if x[5] == ver and x[1] > 0]
        ax_top.scatter(xs, [max(x[3], 1.0) for x in b if x[5] == ver and x[1] > 0],
                       s=8, color=color, label=f"served by v{ver}")
        ax_bot.scatter(xs, [x[4] for x in b if x[5] == ver and x[1] > 0], s=8, color=color)
    width = STREAM_TICKS / 1000.0
    for x in b:
        if x[2] > 0:
            for ax in (ax_top, ax_bot):
                ax.axvspan(x[0] - width, x[0], color=FAIL_COLOR, alpha=0.35, lw=0)
    for t, label in events:
        for ax in (ax_top, ax_bot):
            ax.axvline(t, color="#555", ls=":", lw=1)
        ax_top.text(t, 1500, f" {label}", fontsize=8, color="#333")

    ax_top.set_yscale("log")
    ax_top.set_ylim(1, 3000)
    ax_top.axhline(1000, color="#888", lw=0.8, ls="--")
    ax_top.set_ylabel("slowest read per 20 ms (µs)")
    # The band between ~50 µs and the 1 ms line is empty: legend goes there.
    ax_top.legend(loc="upper right", bbox_to_anchor=(1.0, 0.85), ncol=2, fontsize=8,
                  markerscale=1.5, frameon=False)
    ax_bot.set_ylim(0, 500)
    ax_bot.set_ylabel("largest error per 20 ms (mg)")
    ax_bot.set_xlabel("time (s)")
    for ax in (ax_top, ax_bot):
        ax.set_xlim(0, seconds)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=12, help="client run time (default 12)")
    ap.add_argument("--init-ms", type=int, default=200,
                    help="simulated hardware init time for every driver start (default 200)")
    ap.add_argument("--ssh", default=DEFAULT_SSH, help="ssh command prefix to reach the target")
    ap.add_argument("--remote-dir", default="/tmp/hotswap", help="where make deploy put the files")
    ap.add_argument("--save", help="also save the final figure to this file")
    ap.add_argument("--close", action="store_true",
                    help="close the window when both runs have finished")
    args = ap.parse_args()
    password = os.environ.get("QNX_SUDO_PASSWORD", "qnxuser")

    runs = [
        Run("Hot swap (qnx-hotswap)", "swap", "/dev/accel"),
        Run("Stop and restart (conventional driver)", "restart", "/dev/accel_restart"),
    ]
    for r in runs:
        r.start(args.ssh, args.seconds, args.init_ms, password, args.remote_dir)

    fig, axes = plt.subplots(2, len(runs), figsize=(13, 7), sharex=True,
                             gridspec_kw={"height_ratios": [3, 2]})
    fig.suptitle("Live: 1 kHz client reading a simulated accelerometer on QNX 8.0 "
                 "during driver upgrades", fontsize=12)
    saved = {"done": False}

    def update(_frame):
        # Check completion before drawing, so the saved frame says "finished".
        all_done = all(r.done for r in runs)
        for col, r in enumerate(runs):
            draw_run(axes[0][col], axes[1][col], r, args.seconds)
        fig.tight_layout()
        if not saved["done"] and all_done:
            saved["done"] = True
            if args.save:
                fig.savefig(args.save, dpi=150)
                print(f"saved {args.save}", flush=True)
            if args.close:
                plt.close(fig)

    anim = FuncAnimation(fig, update, interval=200, cache_frame_data=False)  # noqa: F841
    plt.show()
    for r in runs:
        if r.proc.poll() is None:
            r.proc.terminate()


if __name__ == "__main__":
    main()
