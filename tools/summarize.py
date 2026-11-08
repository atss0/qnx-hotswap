#!/usr/bin/env python3
"""Summarise a series of demo runs and decide PASS or FAIL.

Input: the directory filled by scripts/run-all.sh, with for every run
    <scenario>-<i>.csv        per-tick samples of the first client
    <scenario>-<i>-c<n>.csv   samples of further clients (CLIENTS=n)
    <scenario>-<i>.log        the demo's output

Writes summary.md and summary.csv next to the data and prints the table.

What is checked:
  - failed reads and sequence breaks for each client,
  - with several clients, that the sequence numbers all clients received,
    merged, have no hole and no duplicate (each client alone sees only part
    of them, so this is the only check that can notice a lost sample),
  - the expected outcome of each scenario (see EXPECT).

No third-party packages are needed.
"""
import csv
import glob
import os
import re
import statistics
import sys
from collections import defaultdict

# What each scenario must look like, per run. Keys:
#   failed:    maximum failed reads per client (None: not a criterion)
#   breaks:    maximum sequence breaks per client (None: not a criterion)
#   reconnects: required number of re-opens per client (None: not a criterion)
#   exit:      True if the demo must exit with code 0
EXPECT = {
    "none":        dict(failed=0, breaks=0, reconnects=0, exit=False),
    "swap":        dict(failed=0, breaks=0, reconnects=0, exit=False),
    "rollback":    dict(failed=0, breaks=0, reconnects=0, exit=False),
    "late-crash":  dict(failed=0, breaks=0, reconnects=0, exit=False),
    "hang":        dict(failed=0, breaks=0, reconnects=0, exit=False),
    "rogue":       dict(failed=0, breaks=0, reconnects=0, exit=False),
    # An unplanned kill is an outage, but the descriptor must survive and the
    # state must come back (at most one small step back in the sequence).
    "kill":        dict(failed=None, breaks=1, reconnects=0, exit=False),
    # Baselines: measured, nothing to pass.
    "restart":     dict(failed=None, breaks=None, reconnects=None, exit=False),
    "restart-mbb": dict(failed=None, breaks=None, reconnects=None, exit=False),
}

TOTAL_RE = re.compile(r"TOTAL reads=(\d+) failed=(\d+) seq_breaks=(\d+) uncal=(\d+) missed=(\d+)")
RECONNECT_RE = re.compile(r"TOTAL reconnects=(\d+) longest_outage_ms=([\d.]+)")
SWAP_RE = re.compile(r"switch took ([\d.]+) us")
ROLLBACK_RE = re.compile(r"restored from .* in ([\d.]+) us")


def percentile(sorted_values, q):
    if not sorted_values:
        return float("nan")
    i = min(len(sorted_values) - 1, int(q * len(sorted_values)))
    return sorted_values[i]


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def scenario_base(name):
    return re.sub(r"-\d+c$", "", name)


def analyse_run(scenario, run, directory):
    """Return the numbers of one run, or None if its files are incomplete."""
    log_path = os.path.join(directory, f"{scenario}-{run}.log")
    with open(log_path, errors="replace") as f:
        log = f.read()

    paths = [os.path.join(directory, f"{scenario}-{run}.csv")]
    paths += sorted(glob.glob(os.path.join(directory, f"{scenario}-{run}-c*.csv")))
    rows_per_client = [read_csv(p) for p in paths if os.path.exists(p)]

    totals = [tuple(map(int, m)) for m in TOTAL_RE.findall(log)]
    reconnects = RECONNECT_RE.findall(log)

    reads = [float(r["read_us"]) for rows in rows_per_client for r in rows if r["ok"] == "1"]

    # The client-side cost of a swap: the read during which the serving
    # version changed (first client only, a clean single sequence).
    swap_reads = []
    first = rows_per_client[0] if rows_per_client else []
    prev = None
    for r in first:
        if r["ok"] != "1":
            continue
        v = r["version"]
        if prev is not None and v != prev:
            swap_reads.append(float(r["read_us"]))
        prev = v

    # Merged sequence check, only meaningful with several clients.
    holes = dups = None
    if len(rows_per_client) > 1:
        seqs = sorted(int(r["seq"]) for rows in rows_per_client for r in rows if r["ok"] == "1")
        dups = sum(1 for a, b in zip(seqs, seqs[1:]) if a == b)
        holes = sum(b - a - 1 for a, b in zip(seqs, seqs[1:]) if b > a + 1)

    code = re.search(r"exit code: (\d+)", log)
    return {
        "reads": reads,
        "swap_reads": swap_reads,
        "failed": [t[1] for t in totals],
        "breaks": [t[2] for t in totals],
        "uncal": [t[3] for t in totals],
        "missed": [t[4] for t in totals],
        "reconnects": [int(r[0]) for r in reconnects],
        "outage_ms": [float(r[1]) for r in reconnects],
        "switch_us": [float(x) for x in SWAP_RE.findall(log)],
        "rollback_us": [float(x) for x in ROLLBACK_RE.findall(log)],
        "holes": holes,
        "dups": dups,
        "complete": len(totals) >= 1 and len(totals) == len(rows_per_client),
        "exit": int(code.group(1)) if code else None,
    }


def check(scenario, run):
    """Return a list of reasons why this run does not meet the expectation."""
    want = EXPECT.get(scenario_base(scenario))
    if want is None:
        return [f"no expectation defined for {scenario}"]
    problems = []
    if not run["complete"]:
        problems.append("incomplete output")
    if want["failed"] is not None and max(run["failed"], default=0) > want["failed"]:
        problems.append(f"failed reads {run['failed']}")
    if want["breaks"] is not None and max(run["breaks"], default=0) > want["breaks"]:
        problems.append(f"sequence breaks {run['breaks']}")
    if want["reconnects"] is not None and max(run["reconnects"], default=0) > want["reconnects"]:
        problems.append(f"reconnects {run['reconnects']}")
    if want["breaks"] == 0 and (run["holes"] or run["dups"]):
        problems.append(f"merged sequence: {run['holes']} holes, {run['dups']} duplicates")
    if max(run["uncal"], default=0) > 0 and want["failed"] == 0:
        problems.append(f"uncalibrated samples {run['uncal']}")
    return problems


def fmt_range(values, digits=0):
    if not values:
        return "-"
    return f"{min(values):.{digits}f}-{max(values):.{digits}f}"


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "results/latest"
    runs = defaultdict(dict)
    for log in glob.glob(os.path.join(directory, "*.log")):
        m = re.fullmatch(r"(.+)-(\d+)\.log", os.path.basename(log))
        if m:
            runs[m.group(1)][int(m.group(2))] = None
    if not runs:
        sys.exit(f"{directory}: no runs found")

    order = list(EXPECT)
    def key(name):
        base = scenario_base(name)
        return (order.index(base) if base in order else 99, name)

    rows = []
    all_ok = True
    print_problems = []
    for scenario in sorted(runs, key=key):
        results = {i: analyse_run(scenario, i, directory) for i in sorted(runs[scenario])}
        reads = sorted(x for r in results.values() for x in r["reads"])
        problems = {i: check(scenario, r) for i, r in results.items()}
        passed = sum(1 for p in problems.values() if not p)
        ok = passed == len(results)
        all_ok = all_ok and ok
        for i, p in problems.items():
            for reason in p:
                print_problems.append(f"{scenario} run {i}: {reason}")

        per_run = lambda k: [max(r[k], default=0) for r in results.values()]
        flat = lambda k: [x for r in results.values() for x in r[k]]
        holes = [r["holes"] for r in results.values() if r["holes"] is not None]
        dups = [r["dups"] for r in results.values() if r["dups"] is not None]
        is_baseline = EXPECT.get(scenario_base(scenario), {}).get("failed", 0) is None and \
            EXPECT.get(scenario_base(scenario), {}).get("reconnects", 0) is None
        rows.append({
            "scenario": scenario,
            "runs": len(results),
            "failed reads": fmt_range(per_run("failed")),
            "seq breaks": fmt_range(per_run("breaks")),
            "missed deadlines": fmt_range(per_run("missed")),
            "longest outage ms": fmt_range(per_run("outage_ms"), 1),
            "reconnects": fmt_range(per_run("reconnects")),
            "read p50 us": f"{percentile(reads, 0.50):.1f}",
            "read p99 us": f"{percentile(reads, 0.99):.1f}",
            "read max us": f"{reads[-1]:.1f}" if reads else "-",
            "read during swap us (p50/max)":
                f"{statistics.median(flat('swap_reads')):.1f} / {max(flat('swap_reads')):.1f}"
                if flat("swap_reads") else "-",
            "switch us": fmt_range(flat("switch_us"), 1),
            "rollback us": fmt_range(flat("rollback_us"), 1),
            "merged holes/dups": f"{sum(holes)}/{sum(dups)}" if holes else "-",
            "result": "baseline" if is_baseline else ("PASS" if ok else f"FAIL ({passed}/{len(results)})"),
        })

    columns = list(rows[0])
    lines = ["| " + " | ".join(columns) + " |", "|" + "---|" * len(columns)]
    for r in rows:
        lines.append("| " + " | ".join(str(r[c]) for c in columns) + " |")
    table = "\n".join(lines)
    print(table)
    for p in print_problems:
        print("problem:", p)
    print("\nOVERALL:", "PASS" if all_ok else "FAIL")

    with open(os.path.join(directory, "summary.md"), "w") as f:
        f.write(table + "\n")
    with open(os.path.join(directory, "summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, columns)
        w.writeheader()
        w.writerows(rows)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
