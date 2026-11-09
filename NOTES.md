# qnx-hotswap: working notes

Entry for the QNX Global Hackathon (deadline 2026-12-15, 14:00 ET).

## Idea
Upgrade a QNX resource-manager driver while it runs, without clients noticing.
A stable shell process owns the path and channel and forwards messages to a
swappable core process. On upgrade the shell queues requests, v1 hands its state
to v2, the shell switches to v2, and v1 exits.

Demo: a 1 kHz control loop reads a simulated sensor driver during upgrades and
failures, compared with two conventional upgrade baselines.

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
- A boot can take 100 s or more with WHPX; ssh is slow to connect, so `scripts/run-series.sh` runs a whole series in one session

## Testing
`make deploy`, then `scripts/run-all.sh [runs]` on the host: every scenario on
the target, raw data in `results/latest`, summary with PASS/FAIL from
`tools/summarize.py`. `/tmp` files on the target can stay owned by root after
a sudo run: use new file names rather than reusing them.

## Next steps
See "Limitations and next steps" in the README. Short list: per-open (OCB)
state, `ionotify`/blocking reads, shared-memory checkpoints, a real sensor.
