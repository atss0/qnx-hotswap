# qnx-hotswap: submission text (draft)

**Tagline:** Upgrade a running QNX driver without its clients noticing: no
downtime, no lost state, no missed deadlines.

## Inspiration

QNX runs drivers as ordinary user-space processes, so a faulty driver can
be restarted without rebooting. But a restart is still an outage: open file
descriptors die, the device path disappears while the new process
initialises, and everything the driver had learned (calibration, filter
history, counters) is gone. For a car or a robot, "the IMU disappeared for
700 ms and came back uncalibrated" is not an acceptable way to ship a fix.
Live update exists in research systems (MINIX 3 / PROTEOS) by modifying the
OS; we wanted it on a stock QNX 8.0, in user space, with real-time numbers.

## What it does

- A small, stable **shell** process owns the device path (e.g. `/dev/accel`)
  and forwards each client request to a swappable **core** process.
- A new driver version starts and initialises while the old one keeps
  serving, then registers with the shell. The shell exports the old core's
  state, imports it into the new one (converting between state formats when
  the layout changed) and switches over in microseconds.
- The old core is kept on probation: if the new version crashes, the shell
  restores the old core from its own snapshot and retries the failed
  request, so the client still sees no error.
- Clients need no changes and keep their file descriptors.

## Results (QNX 8.0, QEMU with hardware acceleration, 1 kHz client)

| | hot swap | conventional restart |
|---|---|---|
| failed reads per upgrade | **0** | 701 |
| outage | **0 ms** | 701 ms (init + recalibration) |
| driver state | kept (calibration, filter, sequence) | lost |
| switch time | 3-40 µs | - |
| missed deadlines | 0 | 0 (but no data for 701 ms) |

Also: downgrade v2 → v1 with state format conversion, automatic rollback
after a crash (crashed read took 272 µs, no error), four concurrent clients
with 0 failures.

## How we built it

- C on QNX 8.0 with the resource manager framework (`iofunc`/`dispatch`),
  native message passing (`MsgSend`/`MsgReceive`), pulses and timers.
- The shell is single-threaded on purpose: while a swap runs, client
  requests wait SEND-blocked in the kernel, which gives request queueing for
  free; QNX priority inheritance runs the swap at the registering core's
  priority, above the clients'.
- A driver is three callbacks (`import_state`, `export_state`, `read`); the
  same source builds as a hot-swappable core or as a conventional resource
  manager, so comparisons use identical driver code.
- Test clients run a 1 kHz loop on a periodic timer pulse at SCHED_FIFO 50
  and check failed reads, sequence continuity, calibration, measurement
  error against the simulated sensor's known truth, and deadlines.
- QEMU on Windows with WHPX acceleration; Python/matplotlib for plots and a
  live side-by-side viewer.

## Challenges

- Timing in a VM: inside WSL2, QEMU had no hardware acceleration. With WHPX
  it did, but `kernel-irqchip=off` made the guest lose timer interrupts (a
  1 kHz timer ran at ~450 Hz), and 4 vCPUs at normal host priority caused
  random millisecond stalls. Control runs without any swap separated VM
  noise from real effects; 2 vCPUs at High priority fixed it.
- `clock_nanosleep` on QNX rounds up to the clock tick (always ~1 ms late at
  a 1 ms period); periodic timer pulses are the right tool.
- Making the comparison fair: the restart baseline runs the same driver code
  with the same simulated hardware initialisation time.

## What we learned

QNX's message passing already contains most of what live update needs:
blocking sends queue requests, priority inheritance orders the work, and a
resource manager can stand in front of any process.

## What's next

- Transfer per-open (OCB) state, so drivers with per-client sessions can be
  swapped.
- Keep state in shared memory so even an unplanned crash loses nothing.
- Run on real hardware with a real sensor.
- Investigate the occasional one-tick wakeup delay seen with four clients
  (present in control runs without swaps as well).

## Built with

QNX SDP 8.0, C, QNX resource manager framework, QEMU/WHPX, Python, matplotlib.
