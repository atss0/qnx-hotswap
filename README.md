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

## License

Apache License 2.0, see [LICENSE](LICENSE).
