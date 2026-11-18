#ifndef FREETHEWHEEL_FFBIPC_H
#define FREETHEWHEEL_FFBIPC_H

// Same-user Unix-domain socket for the ETS2/ATS telemetry bridge.
//
// Legacy one-shot commands (newline optional):
//   PING, STATUS, SHARED_PROBE, PROBE, FORCE <signed-intensity -32..32>, STOP
//
// Persistent telemetry session (newline-delimited):
//   HELLO FTW_TELEMETRY 1 <ETS2|ATS>
//   T 1 <sequence> <sender_monotonic_ms> <speed_mps> <steering> <rpm> <DRIVING|PAUSED|STOPPED>
//   STOP
//
// The telemetry stream is validated and retained by the IPC server. This
// transport layer does not yet translate telemetry into force feedback.
// Legacy FORCE remains short-lived and is protected by a 2-second watchdog.
// Telemetry sessions are neutralized on pause/stop, disconnect, or >300 ms
// without a valid sample. The DFGT HID remains owned by FreeTheWheel only.
bool StartFFBIPCServer();
const char *FFBIPCSocketPath();

#endif
