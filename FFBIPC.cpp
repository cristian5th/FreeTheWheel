#include "FFBIPC.h"
#include "WheelSupports.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string socketPath;

static bool prepareSocketPath()
{
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    std::string dir = std::string(home) + "/Library/Application Support/FreeTheWheel";
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return false;
    chmod(dir.c_str(), 0700);
    socketPath = dir + "/ffb.sock";
    return socketPath.size() < sizeof(((sockaddr_un *)0)->sun_path);
}

static void replyLine(int fd, const char *line)
{
    (void)send(fd, line, strlen(line), 0);
}

static void *serverMain(void *)
{
    if (!prepareSocketPath()) {
        fprintf(stderr, "FFB IPC: cannot prepare private socket directory.\n");
        return nullptr;
    }

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        perror("FFB IPC socket");
        return nullptr;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strlcpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path));
    unlink(address.sun_path);
    if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        chmod(address.sun_path, 0600) != 0 || listen(server, 4) != 0) {
        perror("FFB IPC bind/listen");
        close(server);
        unlink(address.sun_path);
        return nullptr;
    }

    printf("FFB IPC listening at %s\n", socketPath.c_str());
    for (;;) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("FFB IPC accept");
            break;
        }

        uid_t peerUID = (uid_t)-1;
        gid_t peerGID = (gid_t)-1;
        if (getpeereid(client, &peerUID, &peerGID) != 0 || peerUID != getuid()) {
            replyLine(client, "ERROR unauthorized\n");
            close(client);
            continue;
        }

        char request[128]{};
        ssize_t n = recv(client, request, sizeof(request) - 1, 0);
        if (n > 0) {
            request[strcspn(request, "\r\n")] = '\0';
            if (strcmp(request, "PING") == 0) {
                replyLine(client, "PONG protocol=1\n");
            } else if (strcmp(request, "STATUS") == 0) {
                char status[256];
                snprintf(status, sizeof(status),
                         "STATUS device=DFGT pid=C29A manager_open=%s manager_result=%08x device_ref=%s device_open=%s ffb=disabled\n",
                         IsHIDManagerOpen() ? "yes" : "no",
                         (unsigned int)HIDManagerOpenResult(),
                         IsNativeDFGTAvailable() ? "retained" : "unavailable",
                         IsNativeDFGTDeviceOpen() ? "yes" : "no");
                replyLine(client, status);
            } else if (strcmp(request, "PROBE") == 0) {
                if (!IsNativeDFGTAvailable()) {
                    replyLine(client, "PROBE open=not-attempted result=no_device device_open=no\n");
                } else {
                    IOReturn openResult = OpenNativeDFGTDevice();
                    if (openResult != kIOReturnSuccess) {
                        char response[128];
                        snprintf(response, sizeof(response),
                                 "PROBE open=failed result=%08x device_open=%s\n",
                                 (unsigned int)openResult,
                                 IsNativeDFGTDeviceOpen() ? "yes" : "no");
                        replyLine(client, response);
                    } else {
                        IOReturn closeResult = CloseNativeDFGTDevice();
                        char response[160];
                        snprintf(response, sizeof(response),
                                 "PROBE open=success close=%s close_result=%08x device_open=%s\n",
                                 closeResult == kIOReturnSuccess ? "success" : "failed",
                                 (unsigned int)closeResult,
                                 IsNativeDFGTDeviceOpen() ? "yes" : "no");
                        replyLine(client, response);
                    }
                }
            } else if (strncmp(request, "FORCE ", 6) == 0 || strcmp(request, "STOP") == 0) {
                replyLine(client, "ERROR force_output_not_enabled\n");
            } else {
                replyLine(client, "ERROR unknown_command\n");
            }
        }
        close(client);
    }

    close(server);
    unlink(socketPath.c_str());
    return nullptr;
}

bool StartFFBIPCServer()
{
    pthread_t thread;
    int result = pthread_create(&thread, nullptr, serverMain, nullptr);
    if (result != 0) {
        fprintf(stderr, "FFB IPC: pthread_create failed (%d).\n", result);
        return false;
    }
    pthread_detach(thread);
    return true;
}

const char *FFBIPCSocketPath()
{
    if (socketPath.empty() && !prepareSocketPath()) return nullptr;
    return socketPath.c_str();
}
