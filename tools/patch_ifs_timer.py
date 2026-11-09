#!/usr/bin/env python3
"""Make a copy of the QNX quickstart boot image for the Windows hypervisor.

The quickstart image starts the kernel with `startup-x86 -f 0,0,1000M`, which
tells QNX that the local APIC timer runs at 1 GHz, as it does in QEMU's own
emulation. Under the Windows Hypervisor Platform that timer runs at 200 MHz,
so the guest's clock runs five times too slow: `sleep 10` takes 50 s, a
"1 kHz" loop really runs at 200 Hz, and every measured time comes out five
times too small.

This writes a copy with the frequency changed to 200 MHz. The text keeps its
length (the first field becomes "00", still zero), so nothing else in
the image moves. Afterwards `pidin info` should show the real CPU speed.

Usage: patch_ifs_timer.py <ifs.bin> <output>
"""
import sys

OLD = b"0,0,1000M\0"
NEW = b"00,0,200M\0"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        image = f.read()
    count = image.count(OLD)
    if count != 1:
        sys.exit(f"expected the timer option exactly once, found it {count} times")
    print(f"patching at offset {image.index(OLD):#x}")
    with open(sys.argv[2], "wb") as f:
        f.write(image.replace(OLD, NEW))
    print(f"wrote {sys.argv[2]}")


if __name__ == "__main__":
    main()
