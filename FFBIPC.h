#ifndef FREETHEWHEEL_FFBIPC_H
#define FREETHEWHEEL_FFBIPC_H

// Same-user Unix-domain socket for the ETS2 telemetry bridge.
// Commands: PING, STATUS, FORCE <signed-intensity -32..32>, STOP.
// FORCE is a short-lived constant-force command; a 2-second watchdog sends STOP.
bool StartFFBIPCServer();
const char *FFBIPCSocketPath();

#endif
