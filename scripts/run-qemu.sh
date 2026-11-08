#!/usr/bin/env bash
# Boot the QNX SDP 8.0 quickstart image in QEMU, headless, without root.
#
# Differences from `mkqnximage --run`:
#   - user-mode networking (no bridge, no sudo); guest SSH is forwarded to
#     127.0.0.1:${QNX_SSH_PORT:-2222} on the host
#   - uses KVM when /dev/kvm is usable, otherwise falls back to TCG emulation
#   - serial console goes to output/qemu.out
#
# Usage: scripts/run-qemu.sh [start|stop|status|log]
set -euo pipefail

IMAGE_DIR="${QNX_IMAGE_DIR:-$HOME/qnx-qemu/qemu}"
SSH_PORT="${QNX_SSH_PORT:-2222}"
CPUS="${QNX_CPUS:-4}"
RAM="${QNX_RAM:-1G}"
PIDFILE="$IMAGE_DIR/output/qemu.pid"
LOG="$IMAGE_DIR/output/qemu.out"

is_running() {
    [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

kvm_usable() {
    # /dev/kvm can exist even when the kernel cannot use it (e.g. WSL2 without
    # nested virtualization), so also require the CPU virtualization flag.
    [[ -w /dev/kvm ]] && grep -qE 'vmx|svm' /proc/cpuinfo
}

start() {
    if is_running; then
        echo "QEMU already running (pid $(cat "$PIDFILE"))"
        return 0
    fi

    local accel=(-cpu max)
    if kvm_usable; then
        accel=(-enable-kvm -cpu host)
        echo "Using KVM acceleration"
    else
        echo "KVM not available: using TCG emulation (slower, timing is not representative)"
    fi

    rm -f "$LOG"
    cd "$IMAGE_DIR"
    qemu-system-x86_64 \
        -smp "$CPUS" "${accel[@]}" -m "$RAM" \
        -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22" \
        -device virtio-net-pci,netdev=net0 \
        -object rng-random,filename=/dev/urandom,id=rng0 \
        -device virtio-rng-pci,rng=rng0 \
        -kernel output/ifs.bin \
        -serial "file:$LOG" \
        -display none -monitor none \
        -pidfile "$PIDFILE" -daemonize

    # Wait for the boot to reach a login prompt or a shell.
    for _ in $(seq 1 60); do
        if grep -qiE 'login:|^# ?$' "$LOG" 2>/dev/null; then
            echo "QNX booted. SSH: ssh -p ${SSH_PORT} root@127.0.0.1"
            return 0
        fi
        sleep 5
    done
    echo "Timed out waiting for boot; last console lines:"
    tail -20 "$LOG" || true
    return 1
}

stop() {
    if is_running; then
        kill "$(cat "$PIDFILE")"
        rm -f "$PIDFILE"
        echo "QEMU stopped"
    else
        echo "QEMU not running"
    fi
}

case "${1:-start}" in
    start) start ;;
    stop) stop ;;
    status) if is_running; then echo "running (pid $(cat "$PIDFILE"))"; else echo "stopped"; fi ;;
    log) tail -40 "$LOG" ;;
    *) echo "usage: $0 [start|stop|status|log]"; exit 2 ;;
esac
