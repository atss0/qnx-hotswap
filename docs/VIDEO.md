# Demo video

`qnx-hotswap-demo.mp4`, 2:57 (limit 3:00), 1920x1080. Made with Remotion
outside this repository; every number on screen and in the voice-over is
computed from the measurement series in `results/series-2026-11-13`
(`summary.csv`, the demo logs and the per-tick samples of run 1), nothing is
typed by hand. The voice is Microsoft's neural TTS (en-US-AndrewNeural).

| Scene | Picture | Voice-over |
|---|---|---|
| title | Title card | This is Q N X hot swap. |
| problem | A control loop reading `/dev/accel`; the driver is stopped, the descriptor dies, the outage bar fills (init 200 ms + calibration 250 ms) | On QNX, drivers are ordinary processes, so a fix ships by restarting the driver. But a restart is an outage: file descriptors die, the device vanishes while the new version initialises, and its calibration is gone. |
| arch | Architecture: client, shell (owns `/dev/accel`), core v1 and v2; the state moves from v1 to v2 through the shell | Q N X hot swap splits a driver in two. A small shell owns the device path and never restarts; the driver and its state live in a core behind it. A new version initialises in the background and registers, then the shell moves the state across and switches over, in microseconds. |
| queue | Requests queue SEND-blocked in the kernel during the switch (illustration, slowed down) | Requests that arrive meanwhile wait in the kernel's message queue, and priority inheritance runs the swap above the clients. Clients keep their file descriptor. |
| api | `src/driver.h`: the three callbacks; the same `accel.c` builds as core and as conventional driver | A driver is three callbacks: import state, export state, and read. The same source also builds as a conventional driver, so every comparison runs identical code. |
| live | Recorded run (`swap-1`, `restart-1` from the series) replayed side by side: read time and measurement error | A simulated accelerometer, read by a one kilohertz control loop. Left: hot swap. Right: the same driver, restarted the usual way. At three seconds, version two goes in. On the right, the device is gone for 458 milliseconds: init plus recalibration. On the left, zero failed reads. |
| glitch | Same charts to 10 s: v2's glitch rejection removes the error spikes; downgrade at 6 s | Version two also rejects sensor glitches, so the error spikes vanish. That needed new state fields, and the old state is converted on the fly. At six seconds we go back to version one: a downgrade is just another swap. |
| real | Screen recording on the QNX target (1.7x speed): `uname`, `pidin` with v1 and v2 alive during probation, the commit, the client's TOTAL line | And this is the real thing, on the QNX target. During probation both versions are alive. |
| rollback | Terminal: real output of `demo-swap.sh 10 rollback` (run 1 of the series), shortened | If the new version crashes, the old one, kept on probation, is restored and the same request retried. The client sees one slow read, and no error. |
| failures | Four cards: hang, late crash, kill after probation, rogue process | A core that hangs is caught by a kernel timeout, at the cost of 3 missed deadlines at most. A crash after five hundred reads loses no samples, because state is checkpointed during probation. A kill after probation is an outage, but descriptors survive and the state comes back. And no other process can command a core. |
| results | Measured: switch time, 0 failed reads, 0 lost state, missed deadlines vs. control, 4 clients, restart outage | Over ten runs of every scenario: swaps take 44 to 106 microseconds, with zero failed reads and no lost state, even with four clients. A plain restart costs 458 milliseconds. |
| qnx | The QNX features used: resource managers, message passing, priority inheritance, kernel notifications | It is built only from what QNX already offers: resource managers, message passing, priority inheritance, and kernel notifications. |
| end | Name, tagline, repository URL, license | Q N X hot swap. Upgrade the driver, not the downtime. Open source, on GitHub. |

All charts and terminal lines come from real runs on QNX 8.0 (QEMU + WHPX);
the chart scenes replay a recorded run slowed down, which the video says on
screen. The `real` scene is an unedited screen recording of a terminal on the
QNX target, played at 1.7x speed.
