#ifndef FREETHEWHEEL_FFBIPC_H
#define FREETHEWHEEL_FFBIPC_H

// Starts a same-user Unix-domain socket server for the ETS2 telemetry bridge.
// Protocol v1: PING, STATUS. FORCE/STOP are intentionally rejected for now.
bool StartFFBIPCServer();
const char *FFBIPCSocketPath();

#endif
