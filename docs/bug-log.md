# Bug log

Every real bug or surprise hit while building sensord, how it showed up, and
which tool pinned it down.

| # | Bug | Found by | Confirmed with | Fix |
|---|-----|----------|----------------|-----|
| 1 | One device unplug counted as two reconnects | Watching the `RECON` column in `sensorctl list` | GDB `dprintf`, a 20-unplug count, `strace` | Close the listen socket before the client socket in `sensord-sim` |
| 2 | ~400 ThreadSanitizer reports in the gRPC tests | TSan | Script that classifies every report by its first in-repo frame | Scoped the TSan CI job (see below) |
| 3 | Bitwise CRC-16 was the top CPU hotspot | `perf record` | `perf report`, micro-benchmark | Table-driven CRC |
| 4 | Dashboard didn't draw "DISCONNECTED" for a device with no data yet | Looking at the dashboard screenshot | Re-render | Draw the overlay even when a lane has no samples |
| 5 | ASan build failed to link (`libclang_rt.asan` missing) | Linker | - | `libclang-rt-18-dev` added to CI deps |

---

## 1. One unplug counted as two reconnects

**Symptom.** With a `sensord-sim --tcp` device, killing the simulator once
sometimes moved `RECON` from 0 to 2 and bumped the connection epoch twice.

**First look with GDB.** Attached to the running server and put a
non-stopping `dprintf` on the return of `TcpDevice::open()` and on the
"link dropped" branch in `DeviceReader::run()`:

```
dprintf tcp_device.cpp:64,"[gdb] TcpDevice::open returns fd_=%d\n", fd_
dprintf device_reader.cpp:143,"[gdb] link dropped (reconnects_ before=%lu)\n", reconnects_._M_i
```

In the optimized build the line breakpoints were unreliable (code moved by
`-O2`), and in a Debug build the double count never happened in 5 tries:
each unplug showed exactly one drop followed by failed `open()`s. So it was
timing-dependent.

**Measuring it.** A script plugged and unplugged the device 20 times against the
release build: **23 drops for 20 unplugs**.

**Finding the window.** `strace -tt -e close` on the simulator at exit:

```
22:58:18.988060 close(4)   = 0     <- client socket
22:58:18.988304 close(3)   = 0     <- listening socket, 244 us later
```

sensord sees EOF on fd 4 and immediately (first retry has no backoff) calls
`connect()`. The listening socket is still open for ~244 us, so the kernel
completes the handshake into its backlog; then the listener closes, the
half-accepted connection is reset, and the reader counts a second drop.
Slowing the simulator down with strace made it happen on every unplug
(30 drops for 15 unplugs).

**Fix.** On shutdown, close the listening socket first, then the client.
After the fix: **20 drops for 20 unplugs**.

**Takeaway.** A real USB device can't do this, but a TCP device that
restarts can. A connection that delivers zero bytes before dropping is a
hint the peer is going away; a future improvement is to not reset the
backoff until the link has delivered at least one valid frame.

## 2. ThreadSanitizer and gRPC

Under `-fsanitize=thread` all unit and stress tests are clean, but every
gRPC end-to-end test reported data races: **396 reports across the 6 E2E
tests**. A short script split the reports and looked at the first stack frame
of each access that wasn't inside gRPC / protobuf / upb / absl: **0 of 396**
started in sensord code. They are all inside gRPC's own threads (epoll
poller, upb parser, absl status), which TSan can't see synchronize because
Ubuntu's `libgrpc++` is not built with TSan.

**Decision.** The TSan CI job runs everything except the gRPC E2E tests. The
code that is ours and concurrent (bounded queue, device readers, hub, shutdown)
is covered by the unit tests and the stress test (8 devices, 20 subscribers,
random connect/disconnect, random unplugs, shutdown under load), none of which
link gRPC. The E2E tests run under ASan + UBSan instead. The proper fix is
building gRPC from source with `-fsanitize=thread`, which takes ~30 min in CI.

## 3. CRC-16 hotspot

See README "Profiling". `perf record` under a saturating load put the
bit-at-a-time `crc16()` at 18% of all CPU samples. Every frame is CRC'd twice
(simulator encode + host decode). A 256-entry table, generated at compile time
by a `constexpr` function, does one lookup per byte instead of 8 shift/xor
steps. A test checks the table version against the old one on 2000 inputs.

## 4. Dashboard overlay

The lane painter did `if (no samples yet) continue;` before drawing the
"DISCONNECTED" label, so a device that was already down when the dashboard
started showed an empty lane with no label. Caught in the README screenshot.
