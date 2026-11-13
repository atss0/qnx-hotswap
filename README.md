# qnx-hotswap

Upgrade a running QNX driver without its clients noticing.

On QNX, drivers are ordinary user-space processes (resource managers) that
clients talk to through message passing. `qnx-hotswap` uses that to replace a
driver with a new version while a real-time client keeps using it:

- a small, stable **shell** process owns the path (for example `/dev/accel`)
  and the channel that clients connect to;
- a swappable **core** process holds the driver logic and its state;
- on upgrade the shell holds incoming requests, the old core hands its state
  to the new one (converting between state formats if the layout changed),
  the shell switches over, and the old core is kept on probation so a crash
  or hang of the new version rolls back automatically.

Clients keep the same file descriptor the whole time. A 1 kHz control loop
sees **no failed reads and no lost state** during the swap, and no more missed
deadlines than in a run without any upgrade. The same driver restarted the
conventional way is gone for 457 ms and recalibrates on every upgrade.

![Architecture: clients, shell, core v1 and v2](docs/architecture.svg)

![Simulated accelerometer at 1 kHz: hot swap, rollback, stop-and-restart](docs/accel-swap-vs-restart.png)

> Status: working prototype for the QNX Global Hackathon (2026).

## Results at a glance

QNX 8.0 on QEMU with WHPX (2 vCPUs), simulated accelerometer, 1 kHz client
at SCHED_FIFO 50, 10 s per run, upgrades at 3 s and 6 s, **10 runs of every
scenario** (`scripts/run-all.sh`, raw data in
[`results/series-2026-11-13`](results/series-2026-11-13)):

| | hot swap | stop and restart | make-before-break restart |
|---|---|---|---|
| failed reads per upgrade | **0** | 456-459 | 1 |
| longest outage | **0 ms** | 456-461 ms | 1.0-2.4 ms |
| client descriptor | **kept** | dies (`EBADF`, reopen) | dies (`EBADF`, reopen) |
| calibration | **kept** | redone every upgrade | reloaded from a file |
| filter history, sequence | **kept** | lost | lost |
| missed deadlines in the upgrade windows | **0-1** | 1-2 | 0 |
| switch time | 44-106 µs (median 76) | - | - |

Without any upgrade (`none`), the same windows show 0-1 missed deadlines:
the VM itself occasionally stalls (see [Measurement notes](#measurement-notes)).

Failure handling, also 10 runs each, every run PASS:

| scenario | what happens | client sees |
|---|---|---|
| `rollback` | new v2 crashes on its first read | 0 failed reads; that read takes ~2 ms instead of ~14 µs; rollback in 13-57 µs |
| `late-crash` | new v2 crashes on its 500th read | 0 failed reads, no sample lost or repeated (resumes from the last checkpoint) |
| `hang` | new v2 spins forever on its first read | 0 failed reads; one read takes about 6 ms (5 ms timeout, then rollback) |
| `kill` | committed v2 is `SIGKILL`ed, no predecessor left | outage until a new core starts (here 1.2 s); descriptor stays valid (`EAGAIN`), calibration and state come back from the checkpoint |
| `rogue` | another process tries `QUIT`/`EXPORT`/`IMPORT` on the core | nothing; 3 of 3 attempts refused with `EPERM` |
| `swap`, `rollback` with 4 clients | four 1 kHz clients at once | 0 failed reads; the sequence numbers of all clients, merged, have no hole and no duplicate |

## How it works

### Swap

A new core starts, initialises (here: 200 ms simulated hardware init) while
the old one keeps serving, and registers with the shell (a `devctl` on the
device). The shell is single-threaded on purpose: while it runs the swap,
client requests wait SEND-blocked in the kernel on its channel, which gives
request queueing for free. The registering thread's priority (`core -p`,
default 60) is inherited by the shell through message passing, so the swap
runs above the clients as long as that priority is above the highest client
priority.

The swap itself: export the old core's state in a format the new core can
read, import it into the new core, switch. Core v2 stores more state than v1
(format 2 adds glitch-rejection fields); v2 converts format 1 on upgrade and
writes format 1 when a downgrade to v1 asks for it. Over the 20 swaps of the
`swap` series the switch took 44-106 µs (median 76 µs); a single swap on an
otherwise idle target, as in the demo video's screen recording, can take
under 20 µs. Anything the driver logs during import is flushed only after
the core has replied, so output stays off the critical path. The client's
read that waits for a swap takes 15 µs at the median and 53 µs at most.

### Probation, rollback and checkpoints

After a swap the old core stays parked for a probation period (`shell -t`,
default 2 s). During probation the new core's state is checkpointed after
every request, in a format the old core can read. If the new core fails, the
shell restores the old core from the last checkpoint and retries the failed
request there, so the client sees no error and no step back in the
sequence. After probation the swap is committed and the old core exits. A new
registration during probation is refused (`EBUSY`): one upgrade at a time.

Failure detection uses the kernel:

- **crash**: a `_PULSE_CODE_COIDDEATH` pulse arrives as soon as a core dies,
  even when nobody is reading, and a pending `MsgSend` fails;
- **hang**: every `MsgSend` to a core has a kernel timeout (`TimerTimeout`):
  5 ms for reads and writes (`shell -r`), 50 ms for state transfers
  (`shell -x`). A core that does not answer in time is killed (after
  checking with `ConnectServerInfo` that the pid still belongs to it) and
  treated as crashed. The timeout must stay above the driver's worst case: a
  false alarm kills a healthy core.

Detecting a crash takes the kernel's process teardown: the read that hits a
crash takes about 2 ms instead of ~14 µs, so a rollback can cost one or two
deadlines of a 1 kHz loop (1-3 missed in the upgrade windows, against 0-1
without upgrades).

### After probation

If the only core is lost, reads return `EAGAIN`: client descriptors stay
valid. The shell keeps a checkpoint of the core's state every 100 ms
(`shell -c`); the next core that registers, started by whatever supervises
the system, continues from it, without recalibrating. Up to 100 ms of state
updates can be lost (in `kill` the sequence steps back once). In the `kill`
scenario the demo script plays the supervisor and starts a new core 1 s after
the kill; the outage is that second plus the 200 ms init.

### Access control

A core's channel must be reachable by the shell, another process, so any
process could connect to it. The core therefore records the shell's pid,
taken from the kernel (`ConnectServerInfo` on its own connection to the
device), and answers every message from another process with `EPERM`. It
exits when the shell goes away (`_PULSE_CODE_DISCONNECT`). `src/rogue.c` is
the attacker used by the `rogue` scenario.

### Why the shell can be trusted

The shell never restarts, so it is a single point of failure. It is kept
small and free of driver logic: about 730 lines, mostly message forwarding,
swap and recovery; it does not know any driver's record or state layout, so
driver fixes never touch it. It can be watched by a supervisor like any
other process; if it dies, the result is the same as a conventional driver
restart (descriptors die, clients reopen), and the next core starts fresh.
Keeping the checkpoint in shared memory would let even a shell restart
preserve the driver state (not implemented).

### What a driver implements

```c
struct driver {
    const char *path;            /* "/dev/accel" */
    uint32_t    version;
    uint16_t    fmt_min, fmt_max; /* state formats it reads / writes */
    int     (*import_state)(const struct core_state *st);
    int     (*export_state)(uint32_t wanted, struct core_state *st);
    ssize_t (*read)(void *buf, size_t max);
    ssize_t (*write)(const void *buf, size_t len);              /* optional */
    ssize_t (*control)(uint32_t dcmd, void *data, size_t in, size_t max); /* optional */
};
```

The same driver source builds as a swappable core (`core_main.c`) and as a
conventional single-process resource manager (`mono_main.c`), so every
comparison runs identical driver code.

## Code layout

| File | Role |
|---|---|
| `src/shell.c` | the stable resource manager: owns the path, forwards reads, writes and driver `devctl`s, runs swaps, probation, rollback, checkpoints and recovery. Knows nothing about the driver. |
| `src/core_main.c` | host process for a swappable core: registers with the shell, serves only the shell's requests through the driver callbacks |
| `src/mono_main.c` | host for the same driver as a conventional resource manager (the restart baselines, including make-before-break) |
| `src/driver.h`, `src/hotswap_proto.h` | the driver interface and the shell-core protocol |
| `src/accel.c`, `src/accel.h` | accelerometer driver with simulated hardware: calibration, moving-average filter, v2 adds glitch rejection |
| `src/counter.c`, `src/counter.h` | minimal counter driver |
| `src/accel_client.c`, `src/client.c` | 1 kHz test clients for the two drivers |
| `src/rogue.c`, `src/ctl_test.c` | access-control attacker; `write`/`devctl` forwarding test |
| `scripts/demo-swap.sh` | runs one scenario on the target |
| `scripts/run-all.sh`, `scripts/run-series.sh` | every scenario N times, raw data back to the host, summary |
| `tools/summarize.py` | the PASS/FAIL table (no third-party packages) |
| `tools/plot.py`, `tools/live.py` | figures, and a live side-by-side view |
| `tools/patch_ifs_timer.py` | fixes the boot image's timer frequency for WHPX (see below) |

The Makefile builds each driver into cores `<drv>_v1`, `<drv>_v2`, test
variants `<drv>_v2_bad` (crashes on its first read), `<drv>_v2_late`
(crashes on its 500th read), `<drv>_v2_hang` (spins on its first read), and
conventional drivers `<drv>_mono_v1`, `<drv>_mono_v2`.

## Building and running

Requirements: QNX SDP 8.0 (`qcc`, tested with 8.0.5 on WSL2 Ubuntu 24.04) and
a QNX 8.0 x86_64 target; this repo uses the SDP QEMU quickstart image.

```sh
source ~/qnx800/qnxsdp-env.sh
make deploy                       # builds into build/, copies to qnxvm:/tmp/hotswap
scripts/run-all.sh 10             # every scenario 10 times (~27 min), PASS/FAIL table

# a single scenario, on the target as root:
cd /tmp/hotswap
DRV=accel INIT_MS=200 ./demo-swap.sh 10 swap   # or rollback, late-crash, hang, kill,
                                               # rogue, restart, restart-mbb, none
```

`CLIENTS=4` runs four clients at once; `SAMPLES=file.csv` writes one line per
client tick, which `tools/plot.py` draws:

```sh
python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python tools/plot.py --run "Hot swap" results/latest/swap-1.csv \
    --run "Rollback (buggy v2)" results/latest/rollback-1.csv \
    --run "Stop and restart" results/latest/restart-1.csv \
    --title "Simulated accelerometer, 1 kHz client on QNX 8.0: two upgrades" \
    --out docs/accel-swap-vs-restart.png
```

`results/series-2026-11-13` holds the series behind every number in this
README: the demo log of every run, `summary.md`/`summary.csv`, and the
per-tick samples of run 1 of every scenario (gzip). `tools/summarize.py` on
a full `results/latest` reproduces the table; the CSVs of the other runs are
left out for size (68 MB).

### What `run-all.sh` checks

For every run of every scenario: failed reads, sequence breaks, re-opens,
uncalibrated samples, and that the driver output never stays wrong (error
above 500 mg) for more than 5 samples in a row. With several clients, the
sequence numbers of all clients merged must have no hole and no duplicate
(each client alone sees only part of them, so this is the only check that
notices a lost sample). `restart` and `restart-mbb` are baselines and only
measured.

## The QNX VM

QEMU inside WSL2 has no hardware acceleration when Windows virtualization-based
security is on, and software emulation makes timing numbers meaningless. So the
VM runs in QEMU for Windows with the Windows Hypervisor Platform (WHPX):

```powershell
winget install SoftwareFreedomConservancy.QEMU
powershell -ExecutionPolicy Bypass -File scripts\run-qemu-win.ps1 start   # stop | status | log
```

The script expects the boot image and `disk-qemu.qcow2` in `D:\qnx-vm`
(`-VmDir` to change). The qcow2 is made once from the quickstart image:

```sh
qemu-img convert -p -O qcow2 ~/qnx-qemu/qemu/output/disk-qemu disk-qemu.qcow2
python3 tools/patch_ifs_timer.py ifs.bin ifs-whpx.bin
```

**The clock fix matters.** The quickstart boot image tells QNX that the local
APIC timer runs at 1 GHz, as in QEMU's own emulation. Under WHPX it runs at
200 MHz, so the guest clock ran five times too slow and every measured time
came out five times too small, with no other visible symptom.
`tools/patch_ifs_timer.py` writes a copy of the image with the right
frequency, `run-qemu-win.ps1` boots it, and `run-all.sh` refuses to measure
if `sleep 5` on the target does not take about 5 s. (Results published
before 2026-11-09 were affected; every number here is from after the fix.)

WHPX must keep the interrupt controller in the hypervisor
(`kernel-irqchip=on`, the script's default); with `off` the guest loses timer
interrupts. Guest SSH is forwarded to `127.0.0.1:2223`; with
`networkingMode=mirrored` in `%USERPROFILE%\.wslconfig`, WSL reaches it at
the same address. `scp` hangs against this image, so files are copied over
`ssh` with `cat`. `scripts/run-qemu.sh` boots the same image inside WSL
(KVM if available, otherwise emulation), with SSH on port 2222.

## Measurement notes

- **Missed deadlines are VM noise first.** Even with no driver activity at
  all (`none`), the 1 kHz client misses 0-12 deadlines per 10 s (one
  `rogue` run, where the attack itself costs nothing measurable, missed 38): the virtual
  CPU is sometimes descheduled by the host for a millisecond or more, and
  in some runs the guest's timer briefly drifts against `CLOCK_MONOTONIC`
  (`examples/jitter` shows the same in plain timer and `clock_nanosleep`
  loops: some runs stay below 0.6 ms lateness, others build up several ms).
  That is why `summarize.py` also counts missed deadlines inside the
  upgrade windows (2.9-4.0 s and 5.9-7.0 s) and every scenario is compared
  with `none` over the same windows.
- **Forwarding cost.** Reads through the shell take 14 µs at the median and
  63 µs at p99, against 9 µs and 47 µs for the same driver as a single
  process: about 5 µs per read for the extra message pass. Read requests
  carry only a header to the core.
- **Four clients.** All four 1 kHz clients go through the single-threaded
  shell, so reads queue behind each other (median 49 µs). Missed deadlines
  in the upgrade windows: 0-3 for `swap` and 1-3 for `rollback`.
- **Restart baseline.** The 457 ms outage is 200 ms of simulated hardware
  init (`INIT_MS`, the same for both designs) plus 250 ms of calibration.
  With `INIT_MS=0` it would be about 250 ms; a make-before-break restart
  avoids most of it but still kills descriptors and loses unsaved state.
- **Rollback cost.** A crash is noticed when the kernel tears the process
  down, which is what makes the affected read take 1.7-2.3 ms.

## Limitations and next steps

- **Scope of forwarding.** `read`, `write` and driver-specific `devctl` are
  forwarded. Not supported: `pread`/`readcond` (`ENOSYS`),
  `ionotify`/`select`/`poll`, blocking reads that the driver replies to
  later, and per-open (OCB) state: the core sees one stream of requests, not
  sessions.
- **Hardware hand-over.** The sensor is simulated as a pure function of time,
  so two cores can sample it at once. A real driver owns an interrupt, device
  memory and possibly DMA, and only one process can own them. The protocol
  would need a short quiesce step: the old core stops the device and calls
  `InterruptDetach`, exports state including in-flight transfers, the new
  core maps the registers and attaches the interrupt, then serves. The
  register state must fit in the state blob (512 bytes here). Not
  implemented; no real sensor was available.
- **Rollback window.** After probation there is no predecessor: a crash then
  is an outage until a supervisor starts a new core (descriptors survive,
  state comes from the 100 ms checkpoint). The demo uses its script as
  supervisor; QNX's High Availability Manager would be the production
  choice, but `ham` is not in the quickstart image.
- **Shell restart** loses the driver state (checkpoint in shared memory: not
  implemented).

## Live side-by-side view

`tools/live.py` runs the hot swap (on `/dev/accel`) and the conventional
restart (on `/dev/accel_restart`) at the same time on the target and plots
both clients live from 20 ms summaries they stream back over SSH. It needs
Python with matplotlib and Tk (it runs on the Windows host) and the binaries
deployed with `make deploy`:

```powershell
python tools\live.py --seconds 12              # window updates while the runs go
python tools\live.py --save docs\live-final.png --close
```

![Live view, final frame](docs/live-final.png)

## License

Apache License 2.0, see [LICENSE](LICENSE).
