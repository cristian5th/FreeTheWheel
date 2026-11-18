# FreeTheWheel telemetry IPC and force model (design)

Status: design only. This document does not change runtime code or HID/input handling.

## Goals and invariants

- One process owns the Logitech Driving Force GT (DFGT) HID device: FreeTheWheel.
- The SCS telemetry plugin for ETS2/ATS never opens, claims, or writes to HID.
- Preserve the existing native-mode setup, 900-degree range, buttons, pedals, and discovery-manager ownership path.
- Keep the existing manual IPC commands (`PING`, `STATUS`, `FORCE -32..32`, `STOP`) compatible during migration.
- A pause, game shutdown, disconnect, malformed stream, stale sample, or I/O error must lead to neutral output.
- Do not claim physical road-force telemetry from the currently registered channels. `truck.effective.steering` is a normalized game steering signal, not a measured steering-wheel angle or tire aligning torque.

## Transport

Use the existing same-user Unix-domain stream socket:

`~/Library/Application Support/FreeTheWheel/ffb.sock`

Retain directory mode 0700, socket mode 0600, and `getpeereid()` same-UID validation.

The plugin opens one persistent connection per game session. The server handles multiple newline-delimited records on that connection until EOF. A connection is a telemetry session; only one active telemetry session is allowed. A newly authenticated session replaces the previous session only after the old session is stopped and closed.

Use bounded line buffering (maximum 256 bytes per record). Reject overlong, incomplete, non-ASCII/control-character, non-finite, or out-of-range fields. Do not let untrusted values reach HID report construction.

## Protocol version 1

All records are ASCII, newline terminated, space separated. Decimal floating-point fields use a dot.

### Client to server

Handshake, once after connect:

`HELLO FTW_TELEMETRY 1 ETS2\n`

or

`HELLO FTW_TELEMETRY 1 ATS\n`

Telemetry sample, at a target 60 Hz (maximum 100 Hz):

`T 1 <seq> <monotonic_ms> <speed_mps> <steering> <rpm> <state>\n`

- `seq`: unsigned monotonically increasing 32-bit sequence number; wrap is permitted.
- `monotonic_ms`: sender monotonic milliseconds; used for diagnostics only (do not compare clock domains).
- `speed_mps`: finite signed speed in m/s, accepted range -100..100.
- `steering`: finite normalized effective steering, accepted range -1..1.
- `rpm`: finite engine RPM, accepted range 0..20000.
- `state`: `DRIVING`, `PAUSED`, or `STOPPED`.

Explicit stop:

`STOP\n`

The plugin sends `STOP` before orderly disconnect and whenever the SCS paused event fires. It also closes the socket on shutdown. If send fails, it closes the socket; server-side session timeout is authoritative.

### Server to client

Handshake response:

`READY 1 <max_rate_hz>\n`

Optional diagnostic response:

`ERR <reason>\n`

Telemetry is one-way after handshake; do not require an ACK for every frame. This avoids blocking the SCS callback. The server may log counters and the last accepted sequence locally.

## Server timing and fail-safe behavior

- Accept no more than 100 samples/second; ignore excess samples rather than queueing them.
- Use the server's own monotonic clock for freshness.
- If no valid DRIVING sample arrives for 300 ms, ramp torque to zero and close/neutralize the FFB effect.
- PAUSED, STOPPED, STOP, EOF, peer replacement, parse failure, or HID write failure immediately requests neutral output; ramp down over at most 50 ms where the device remains writable.
- Preserve the existing 2-second watchdog for the legacy manual `FORCE` command. Telemetry mode has its own tighter 300 ms freshness deadline.
- Never block the SCS callback waiting for an IPC reply. Use nonblocking connect/send with a bounded output buffer; drop stale telemetry samples rather than allowing a backlog.

## Initial force calculation: synthetic centering and damping only

The currently verified SCS channels are speed, effective steering, and engine RPM. They are sufficient for a conservative *synthetic centering feel*, not authentic tire/road force feedback. Do not describe the first implementation as road texture, collision, curb, or tire self-aligning torque.

Let:
- (s) be effective steering clamped to [-1, 1].
- (v = |speed_mps|).
- (s_f) be a low-pass filtered steering value.
- (dot{s_f}) be its derivative in 1/s, calculated from server monotonic sample times and clamped to a safe range.

Speed gain:

`g(v) = clamp(v / 8.0, 0.15, 1.0)`

At standstill or near standstill (v < 0.5 m/s), force is disabled. Between 0.5 and 8 m/s, centering strength increases smoothly with speed; above 8 m/s it is capped.

Normalized synthetic torque:

`u = g(v) * (-0.22 * s_f - 0.035 * clamp(dot_s, -4, 4))`

Then:
1. Apply a small steering deadband (|s_f| < 0.015 => centering term zero).
2. Clamp (u) to [-0.25, 0.25] initially.
3. Apply a slew-rate limit so output cannot jump; initial limit 1.0 normalized unit/second.
4. Apply a short low-pass filter (time constant about 40 ms).
5. Convert the bounded normalized value to the DFGT output encoding only inside the FreeTheWheel HID owner. Do not assume the existing manual constant-force report encoding is a general spring/damper API; verify report semantics and polarity experimentally at low strength before enabling closed-loop output.

The coefficients above are deliberately conservative starting values, not calibrated DFGT constants. Keep them in a config structure so they can be tuned without changing protocol parsing. Provide a master enable and immediate STOP.

## Later improvements (separate validation required)

For richer effects, register and validate additional SCS channels or events for wheel position, chassis/cabin angular motion, individual wheel slip, suspension deflection, and road surface data where the SDK/game exposes them. Only then consider speed-sensitive damping, vibration, bumps, and surface effects. Each effect needs its own bounded amplitude, filtering, and fail-safe tests.

## Implementation sequence

1. Implement persistent IPC session and strict parser in FreeTheWheel; keep legacy commands working.
2. Unit-test parser boundaries, rate limiting, sequence wrap, disconnect, stale timeout, pause, and unauthorized peer rejection.
3. Add plugin nonblocking connection/reconnection and telemetry sender; no HID references in the plugin.
4. Add telemetry-to-torque calculation behind a disabled-by-default feature flag.
5. Validate DFGT output report semantics and polarity at minimal force, with a physical emergency stop available.
6. Test in ETS2, then ATS; verify pause/exit/disconnect all neutralize the wheel.
7. Re-run the regression checks for 900-degree range, every button, pedals, and manual `FORCE 8`/ `STOP`.

No input/HID discovery, button, pedal, native-mode, or 900-degree configuration code should be changed as part of the telemetry IPC implementation.
