# Demo video script (target 2:45, limit 3:00)

Recording setup: QNX VM running (`scripts\run-qemu-win.ps1 start`), binaries
deployed (`make deploy`), one terminal with an SSH session to the target,
`tools\live.py` ready to start. Record the screen at 1080p.

| Time | Picture | Voice-over |
|---|---|---|
| 0:00-0:15 | Title card: "qnx-hotswap: upgrade a running QNX driver without its clients noticing" | On QNX, drivers are ordinary processes. That makes them easy to restart, but a restart still means downtime and lost state. qnx-hotswap upgrades a running driver with neither. |
| 0:15-0:45 | Architecture diagram: client → shell (/dev/accel) → core v1 / core v2 | A small, stable shell process owns the device path. The driver logic and its state live in a separate core process. When a new version starts, it initialises in the background, registers with the shell, and the shell moves the state from the old core to the new one and switches over. Client requests that arrive during the switch simply wait in the kernel's message queue, and QNX priority inheritance runs the swap at the right priority. Clients keep the same file descriptor. |
| 0:45-1:05 | Code: `src/driver.h` (three callbacks) | A driver implements three functions: import state, export state, and read. The same driver source builds as a hot-swappable core or as a conventional resource manager, so the comparison you are about to see runs identical driver code. |
| 1:05-1:50 | `python tools\live.py --seconds 12` running live, both panels filling | The driver is a simulated accelerometer: it calibrates its zero-g offset at start-up and filters noise. Two 1 kHz control loops read it. Left: upgraded with hot swap. Right: the same driver upgraded the usual way, by stopping and restarting it. At three seconds, version 2 goes in. On the right, the device disappears for 700 milliseconds: hardware init plus recalibration, 701 failed reads, and the sample sequence restarts from 1. On the left, nothing: orange means version 2 is serving, with zero failed reads. And notice the error plot: version 1's glitch spikes vanish, because version 2 adds glitch rejection, and the state format changed to support it. |
| 1:50-2:00 | Live view at 6 s: second upgrade (downgrade to v1) | At six seconds we go back to version 1. Version 2 writes its state in the old format; the downgrade is just another swap. |
| 2:00-2:25 | Terminal: `DRV=accel ./demo-swap.sh 10 rollback`, highlight the ROLLBACK line and the TOTAL line | What if the new version is broken? This version 2 crashes on its first read. The old version is kept on probation, so the shell restores it from its state snapshot and retries the same request. The client sees one read that took 270 microseconds instead of 7, and no error. |
| 2:25-2:40 | README results table | Measured on QNX 8.0: swaps take 3 to 40 microseconds, zero failed reads, zero lost state, zero missed deadlines at 1 kHz, also with four clients at once. |
| 2:40-2:50 | GitHub URL, license | qnx-hotswap is open source under Apache 2.0. Thanks for watching. |

Notes:
- Keep the live view on screen for the whole 1:05-2:00 segment; it is the
  core of the demo.
- Numbers in the voice-over are from the runs in the README; if a recorded
  run differs, read the numbers from the screen instead.
