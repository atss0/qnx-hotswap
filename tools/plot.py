#!/usr/bin/env python3
"""Plot client samples: hot swap next to stop-and-restart.

Each input is a CSV written by `client ... samples.csv` (via SAMPLES= in
scripts/demo-swap.sh) with the columns
    t_ms, read_us, late_us, ok, value, version

For each run the figure shows, over time:
  - top:    how long each read took (log scale); failed reads as red marks
  - bottom: the counter value the client received; a drop to 0 means the
            driver's state was lost

Usage:
  tools/plot.py --run "Hot swap" swap.csv --run "Stop and restart" restart.csv \
                --out docs/swap-vs-restart.png
"""
import argparse
import csv
import sys

import matplotlib

matplotlib.use("Agg")  # render to a file; no display needed
import matplotlib.pyplot as plt  # noqa: E402

# One colour per driver version; failures are drawn separately in red.
VERSION_COLORS = {1: "#2f6db5", 2: "#e08a1e"}
FAIL_COLOR = "#d62728"


def load(path):
    """Read a samples CSV into lists of floats/ints, one list per column."""
    cols = {"t_ms": [], "read_us": [], "late_us": [], "ok": [], "value": [], "version": []}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            cols["t_ms"].append(float(row["t_ms"]) / 1000.0)   # plot in seconds
            cols["read_us"].append(float(row["read_us"]))
            cols["late_us"].append(float(row["late_us"]))
            cols["ok"].append(int(row["ok"]))
            cols["value"].append(int(row["value"]))
            cols["version"].append(int(row["version"]))
    if not cols["t_ms"]:
        sys.exit(f"{path}: no samples")
    return cols


def summary(c):
    """One-line result shown under each column title."""
    failed = c["ok"].count(0)
    # A break is any good read whose value is not the last good value + 1
    # (comparing with the last good read also catches resets after outages).
    last_good, resets = None, 0
    for ok, v in zip(c["ok"], c["value"]):
        if ok:
            if last_good is not None and v != last_good + 1:
                resets += 1
            last_good = v
    worst = max(c["read_us"])
    return f"{failed} failed reads, {resets} counter breaks, slowest read {worst:.0f} µs"


def plot_run(ax_lat, ax_val, title, c):
    t, ok = c["t_ms"], c["ok"]

    # Read latency, coloured by the version that answered.
    for ver, color in VERSION_COLORS.items():
        xs = [t[i] for i in range(len(t)) if ok[i] and c["version"][i] == ver]
        ys = [c["read_us"][i] for i in range(len(t)) if ok[i] and c["version"][i] == ver]
        if xs:
            ax_lat.scatter(xs, ys, s=2, color=color, label=f"served by v{ver}")
    fails = [t[i] for i in range(len(t)) if not ok[i]]
    if fails:
        top = 1000.0
        ax_lat.scatter(fails, [top] * len(fails), s=6, marker="|", color=FAIL_COLOR,
                       label=f"failed read ({len(fails)})")
    ax_lat.set_yscale("log")
    ax_lat.set_ylim(1, 2000)
    ax_lat.axhline(1000, color="#888", lw=0.8, ls="--")
    ax_lat.text(t[0], 1100, "1 ms period", fontsize=8, color="#666")
    ax_lat.set_ylabel("read time (µs)")
    ax_lat.set_title(f"{title}\n{summary(c)}", fontsize=10)
    # The band between ~50 µs and the 1 ms line is normally empty: put the
    # legend there so it never covers data.
    ax_lat.legend(loc="upper right", bbox_to_anchor=(1.0, 0.86), fontsize=8,
                  markerscale=4, ncol=3, frameon=False)

    # Counter value received; failed reads are left out (no value).
    for ver, color in VERSION_COLORS.items():
        xs = [t[i] for i in range(len(t)) if ok[i] and c["version"][i] == ver]
        ys = [c["value"][i] for i in range(len(t)) if ok[i] and c["version"][i] == ver]
        if xs:
            ax_val.scatter(xs, ys, s=1, color=color)
    for x in fails:
        ax_val.axvline(x, color=FAIL_COLOR, alpha=0.05, lw=1)
    ax_val.set_ylabel("counter value")
    ax_val.set_xlabel("time (s)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", nargs=2, action="append", metavar=("TITLE", "CSV"),
                    required=True, help="a run to plot (repeatable)")
    ap.add_argument("--out", required=True, help="output image (.png or .svg)")
    args = ap.parse_args()

    runs = [(title, load(path)) for title, path in args.run]
    fig, axes = plt.subplots(2, len(runs), figsize=(6 * len(runs), 6),
                             sharex=True, squeeze=False,
                             gridspec_kw={"height_ratios": [3, 2]})
    for col, (title, c) in enumerate(runs):
        plot_run(axes[0][col], axes[1][col], title, c)
    # Same counter scale for all runs, so a reset is visible side by side.
    vmax = max(max(c["value"]) for _, c in runs)
    for col in range(len(runs)):
        axes[1][col].set_ylim(0, vmax * 1.05)

    fig.suptitle("1 kHz client reading /dev/counter on QNX 8.0 during two driver upgrades",
                 fontsize=12)
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
