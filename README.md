# qnx-hotswap

Upgrade a running QNX driver without its clients noticing.

On QNX, drivers are ordinary user-space processes (resource managers) that
clients talk to through message passing. `qnx-hotswap` uses that to replace a
driver with a new version while a real-time client keeps using it:

- a small, stable **shell** process owns the path (for example `/dev/counter`)
  and the channel that clients connect to;
- a swappable **core** process holds the driver logic and its state;
- on upgrade the shell holds incoming requests, the old core hands its state
  to the new one, the shell switches over, and the old core exits.

Clients keep the same file descriptor the whole time. The goal is a 1 kHz
control loop that misses **zero deadlines** during the swap, compared with a
stop-and-restart upgrade.

> Status: early development for the QNX Global Hackathon (2026).
> The environment and timing baseline are done; the shell/core swap is next.

## Requirements

- QNX SDP 8.0 (`qcc`), tested with 8.0.5 on WSL2 Ubuntu 24.04
- A QNX 8.0 x86_64 target. This repo uses the SDP QEMU quickstart image.

## Running the QNX VM

QEMU inside WSL2 has no hardware acceleration when Windows virtualization-based
security is on, and software emulation makes timing numbers meaningless. So the
VM runs in QEMU for Windows with the Windows Hypervisor Platform (WHPX):

```powershell
winget install SoftwareFreedomConservancy.QEMU
powershell -ExecutionPolicy Bypass -File scripts\run-qemu-win.ps1 start   # stop | status | log
```

The script expects `ifs.bin` and `disk-qemu.qcow2` in `D:\qnx-vm` (`-VmDir` to
change). The qcow2 is made once from the quickstart image:

```sh
qemu-img convert -p -O qcow2 ~/qnx-qemu/qemu/output/disk-qemu disk-qemu.qcow2
```

Guest SSH is forwarded to `127.0.0.1:2223`. With `networkingMode=mirrored` in
`%USERPROFILE%\.wslconfig`, WSL reaches it at the same address.

`scripts/run-qemu.sh` boots the same image inside WSL/Linux (KVM if available,
otherwise emulation), with SSH on port 2222.

## Building and running the examples

```sh
source ~/qnx800/qnxsdp-env.sh
cd examples/jitter
qcc -Vgcc_ntox86_64 -O2 -Wall -o jitter jitter.c
ssh qnxvm 'cat > /tmp/jitter && chmod +x /tmp/jitter' < jitter
ssh qnxvm '/tmp/jitter 5000 1000 50 timer'
```

(`scp` hangs against this image, so files are copied over `ssh` with `cat`.)

## Timing baseline

`examples/jitter` runs a 1 kHz periodic loop for 5000 iterations and measures
how late each wakeup is (QNX 8.0 guest, WHPX, AMD Ryzen 7 4800H):

| Loop | avg late | max late | missed deadlines |
|---|---|---|---|
| periodic timer + pulse, priority 50 | 11.8 µs | 121 µs | 0 |
| `clock_nanosleep`, absolute deadline | 722 µs | 890 µs | 0 |

`clock_nanosleep` is late by roughly one clock tick (1 ms) by design: QNX rounds
sleeps up to the tick. Periodic work should use a timer that delivers a pulse.

WHPX must keep the interrupt controller in the hypervisor
(`kernel-irqchip=on`, the script's default). With `kernel-irqchip=off` the
guest loses timer interrupts and the 1 kHz timer runs at about 450 Hz.

## First swap results

`scripts/demo-swap.sh 30 swap` runs the 1 kHz client (priority 50) for 30 s
while core v2 replaces v1 at 3 s and v1 replaces v2 at 6 s. `none` is the
same run without swaps. VM: 2 vCPUs, QEMU at Windows priority High.

| Run | swap time | read errors | counter gaps | missed deadlines | max late |
|---|---|---|---|---|---|
| none #1 | - | 0 | 0 | 0 | 101 µs |
| swap #1 | 8.2 / 8.5 µs | 0 | 0 | 0 | 190 µs |
| none #2 | - | 0 | 0 | 0 | 155 µs |
| swap #2 | 2.1 / 7.6 µs | 0 | 0 | 0 | 380 µs |

### State format changes and rollback

Core v2 stores more state than v1 (format 2 adds a `generation` field). A new
core registers the state formats it can read; the shell asks the old core for
a format the new one understands, and the new core converts older formats.
The same mechanism handles downgrades (v2 writes format 1 for v1).

After a swap the old core stays parked for a probation period (2 s by
default, `shell -t ms`). If the new core fails meanwhile, the shell restores
the old core from its own state snapshot and retries the failed request
there. `demo-swap.sh 10 rollback` swaps in `core_v2_bad`, which crashes on
its first read:

```
shell: swapped v1 -> v2 (pid 2129954), value=2999, ... switch took 8.6 us
core v2 (pid 2129954): simulated bug, crashing
shell: ROLLBACK: core v2 (pid 2129954) failed (No such process); v1 (pid 2105376) restored at value 2999 in 1.6 us
shell: swapped v1 -> v2 (pid 2146338), value=6000, ... switch took 21.5 us
TOTAL reads=10000 errors=0 gaps=0 missed=0 read_us avg=6.7 max=273.1  late_max_us=174.3  core=v2
```

The client sees no error, no counter gap and no missed deadline: the read
that hit the crash took 273 µs instead of ~7 µs. Limitation: whatever the
failed core changed before crashing is lost; the old core resumes from the
handover snapshot.

### Hot swap vs. stop-and-restart

`restart` mode runs the same counter as a conventional single-process
resource manager (`mono_vN`) and upgrades it the usual way, by stopping the
old process and starting the new one (what a QNX HAM restart action does).
The client reopens the device when its descriptor goes bad, as a robust
application would. Both designs get the same simulated hardware
initialisation time (`INIT_MS=200`), 10 s at 1 kHz, two upgrades:

| | hot swap (`swap`) | stop-and-restart (`restart`) |
|---|---|---|
| failed reads | **0** | 402 (201 per upgrade) |
| longest outage | **0 ms** | 201 ms |
| counter state | kept (3200 → 3201) | **lost** (2999 → 1) |
| client code needed | none | reopen on `EBADF` |
| switch time | 10-33 µs | - |

With `INIT_MS=0` the restart outage is about 1 ms (1 failed read per
upgrade), but the state is still lost and the client still sees `EBADF`.
With hot swap the new version initialises while the old one keeps serving,
so initialisation time never becomes downtime.

With 4 vCPUs at normal host priority the guest saw random 1-4 ms stalls
(1-15 missed deadlines per 30 s) with or without swaps: host preemption of
the VM, not the swap.

## License

Apache License 2.0, see [LICENSE](LICENSE).
