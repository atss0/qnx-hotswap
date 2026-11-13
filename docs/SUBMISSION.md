# qnx-hotswap: submission text

**Tagline:** Upgrade a running QNX driver without its clients noticing: no
failed reads, no lost state, and a rollback when the new version breaks.

## Inspiration

QNX runs drivers as ordinary user-space processes, so a faulty driver can
be restarted without rebooting. But a restart is still an outage: open file
descriptors die, the device path disappears while the new process
initialises, and everything the driver had learned (calibration, filter
history, counters) is gone. For a car or a robot, "the IMU disappeared for
half a second and came back recalibrated" is not a good way to ship a fix.
Live update exists in research systems (MINIX 3 / PROTEOS) by modifying the
OS; we wanted it on a stock QNX 8.0, in user space, with real-time numbers.

## What it does

- A small, stable **shell** process owns the device path (e.g. `/dev/accel`)
  and forwards each client request to a swappable **core** process.
- A new driver version starts and initialises while the old one keeps
  serving, then registers with the shell. The shell exports the old core's
  state, imports it into the new one (converting between state formats when
  the layout changed) and switches over in well under a tenth of a millisecond (median 76 µs).
- The old core is kept on probation, and the new core's state is
  checkpointed after every request: if the new version crashes or hangs, the
  shell restores the old core from the last checkpoint and retries the
  failed request, so the client still sees no error.
- After probation, a lost core is an outage until a supervisor starts a new
  one, but client descriptors stay valid and the state, calibration
  included, comes back from a 100 ms checkpoint.
- Only the shell can command a core: the kernel tells the core who is
  sending, and anything else gets `EPERM`.
- Clients need no changes and keep their file descriptors.

## Results (QNX 8.0, QEMU with WHPX, 1 kHz client, 10 runs per scenario)

| | hot swap | conventional restart | make-before-break restart |
|---|---|---|---|
| failed reads per upgrade | **0** | ~457 | 1 |
| outage | **0 ms** | 456-461 ms (init + recalibration) | 1-2.4 ms |
| client descriptor | **kept** | dies | dies |
| driver state | **kept** (calibration, filter, sequence) | lost | calibration from a file; filter and sequence lost |
| missed deadlines around the upgrades | **0-1** (0-1 in control runs) | 1-2 | 0 |
| switch time | 44-106 µs | - | - |

Failure scenarios, all passing in 10 of 10 runs: crash on the first read
(0 failed reads, the affected read takes ~2 ms), crash after 500 reads (no
sample lost or repeated), hang (caught by a 5 ms kernel timeout, 0 failed
reads), kill after probation (descriptor survives, state restored), a rogue
process commanding the core (refused), and swap and rollback with four
concurrent clients (0 failed reads, merged sequence without holes or
duplicates).

## How we built it

- C on QNX 8.0 with the resource manager framework (`iofunc`/`dispatch`),
  native message passing (`MsgSend`/`MsgReceive`), pulses and timers.
- The shell is single-threaded on purpose: while a swap runs, client
  requests wait SEND-blocked in the kernel, which gives request queueing for
  free; priority inheritance runs the swap at the registering core's
  priority (configurable, above the clients').
- Failure detection is the kernel's: `_PULSE_CODE_COIDDEATH` for crashes,
  `TimerTimeout` on every send for hangs, `ConnectServerInfo` to check who
  is on the other end.
- A driver is three callbacks (`import_state`, `export_state`, `read`, plus
  optional `write` and `control`); the same source builds as a hot-swappable
  core or as a conventional resource manager, so comparisons use identical
  driver code.
- Test clients run a 1 kHz loop on a periodic timer pulse at SCHED_FIFO 50
  and check failed reads, sequence continuity, calibration, measurement
  error against the simulated sensor's known truth, and deadlines.
  `scripts/run-all.sh` runs every scenario ten times and prints a PASS/FAIL
  table.
- QEMU on Windows with WHPX acceleration; Python/matplotlib for plots and a
  live side-by-side viewer.

## Challenges

- **A clock that lied.** Under the Windows hypervisor the quickstart image's
  timer setting made the guest clock run five times too slow, with no
  visible symptom: our first published numbers were five times too small.
  We found it, patched the boot image, and the test runner now refuses to
  measure on a wrong clock.
- **Timing in a VM.** The VM misses some 1 kHz deadlines even when nothing
  happens. Control runs over the same time windows separate VM noise from
  real effects.
- **A bug the tests found.** After recovering from a checkpoint, v2's glitch
  filter compared new samples with stale history and held its output for
  over half a second. A new check (the output must never stay wrong for
  more than 5 samples) caught it; the filter now restarts when a "glitch"
  persists.
- **A fair comparison.** The restart baselines run the same driver code
  with the same initialisation time, including the best conventional
  option, make-before-break.

## What we learned

QNX's message passing already contains most of what live update needs:
blocking sends queue requests, priority inheritance orders the work, and the
kernel reports dead and unresponsive servers. A resource manager can stand in
front of any process.

## What's next

- Hardware hand-over: interrupts, device memory and in-flight transfers
  move from the old core to the new one in a short quiesce step.
- Per-open (OCB) state, `ionotify`, and blocking reads.
- Keep the checkpoint in shared memory so even a shell restart loses
  nothing.
- Run on real hardware with a real sensor.

## Built with

QNX SDP 8.0, C, QNX resource manager framework, QEMU/WHPX, Python, matplotlib,
Remotion (video).
