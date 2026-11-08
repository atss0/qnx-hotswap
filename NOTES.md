# qnx-hotswap: working notes

Entry for the QNX Global Hackathon (deadline 2026-12-15, 14:00 ET).

## Idea
Upgrade a QNX resource-manager driver while it runs, without clients noticing.
A stable shell process owns the path and channel and forwards messages to a
swappable core process. On upgrade the shell queues requests, v1 hands its state
to v2, the shell switches to v2, and v1 exits.

Demo: a 1 kHz control loop reads a sensor driver; a live latency graph shows
0 missed deadlines during the swap, compared with a stop-and-restart baseline.

## Environment (WSL2 Ubuntu)
- SDP 8.0.5: `~/qnx800` (`source ~/qnx800/qnxsdp-env.sh`, `qcc -Vgcc_ntox86_64`)
- QEMU image: `~/qnx-qemu/qemu`
- `scripts/run-qemu.sh start|stop|status|log` (SSH on 127.0.0.1:2222)
- `scripts/setup-ssh-key.py` installs the host key for `qnxuser`
- `ssh qnxvm` alias in `~/.ssh/config`; scp hangs, copy with `ssh qnxvm 'cat > /tmp/x' < x`
- `examples/hello` verified on the target

## Main VM: Windows QEMU + WHPX (2026-11-08)
WSL does not expose AMD `svm` (Windows VBS), so the WSL VM is emulation only.
- Image in `D:\qnx-vm` (`ifs.bin`, `disk-qemu.qcow2`); `scripts/run-qemu-win.ps1`
- SSH 127.0.0.1:2223; `.wslconfig` has `networkingMode=mirrored`; `ssh qnxvm` points here
- Windows OpenSSH needs `-c aes128-ctr` ("Corrupted MAC" otherwise)
- Keep `kernel-irqchip=on`: with `off` timer interrupts are lost
- One boot took 102 s instead of ~25 s: not investigated yet

## Next steps
1. Spike: `/dev/counter` shell + core, client at 1000 reads/s, live v1→v2 swap; measure errors, state continuity, worst-case latency
2. Stop-and-restart baseline, live latency graph
3. Optional: emulation (TCG) timing numbers for comparison
