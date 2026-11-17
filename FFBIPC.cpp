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
#include <cstdint>
#include <ctime>

static std::string socketPath;
static void replyLine(int fd, const char *line);
static pthread_mutex_t ffbMutex = PTHREAD_MUTEX_INITIALIZER;
static bool ffbSessionOpen = false;
static bool ffbActive = false;
static time_t lastForceTime = 0;
static constexpr int kMaxForce = 32;
static constexpr time_t kWatchdogSeconds = 2;

static IOReturn sendStopLocked()
{
    if (!ffbSessionOpen) return kIOReturnSuccess;
    const UInt8 report[7] = {0x13, 0, 0, 0, 0, 0, 0};
    IOReturn result = SendNativeDFGTOutputReport(report, 7);
    ffbActive = false;
    IOReturn closeResult = CloseNativeDFGTDevice();
    ffbSessionOpen = false;
    return result != kIOReturnSuccess ? result : closeResult;
}

static void *watchdogMain(void *)
{
    for (;;) {
        sleep(1);
        pthread_mutex_lock(&ffbMutex);
        if (ffbSessionOpen && ffbActive && time(nullptr) - lastForceTime >= kWatchdogSeconds)
            (void)sendStopLocked();
        pthread_mutex_unlock(&ffbMutex);
    }
    return nullptr;
}

static bool parseForce(const char *request, int *force)
{
    if (strncmp(request, "FORCE ", 6) != 0) return false;
    char *end = nullptr;
    long value = strtol(request + 6, &end, 10);
    if (end == request + 6 || *end != '\0' || value < -kMaxForce || value > kMaxForce)
        return false;
    *force = (int)value;
    return true;
}

static void handleForce(int fd, int force)
{
    pthread_mutex_lock(&ffbMutex);
    if (!IsNativeDFGTAvailable()) {
        replyLine(fd, "ERROR no_device\n");
    } else {
        if (force == 0) {
            IOReturn stopResult = sendStopLocked();
            char response[96];
            snprintf(response, sizeof(response), "FORCE neutral stop_result=%08x\n", (unsigned int)stopResult);
            replyLine(fd, response);
            pthread_mutex_unlock(&ffbMutex);
            return;
        }
        IOReturn result = kIOReturnSuccess;
        if (!ffbSessionOpen) {
            result = OpenNativeDFGTDeviceShared();
            if (result == kIOReturnSuccess) ffbSessionOpen = true;
        }
        if (result == kIOReturnSuccess) {
            const UInt8 report[7] = {0x11, 0x08, (UInt8)(0x80 + force), 0x80, 0, 0, 0};
            result = SendNativeDFGTOutputReport(report, 7);
        }
        if (result == kIOReturnSuccess) {
            ffbActive = force != 0;
            lastForceTime = time(nullptr);
            char response[96];
            snprintf(response, sizeof(response), "FORCE accepted value=%d limit=%d\n", force, kMaxForce);
            replyLine(fd, response);
        } else {
            (void)sendStopLocked();
            char response[96];
            snprintf(response, sizeof(response), "ERROR force_io_result=%08x\n", (unsigned int)result);
            replyLine(fd, response);
        }
    }
    pthread_mutex_unlock(&ffbMutex);
}

static void handleStop(int fd)
{
    pthread_mutex_lock(&ffbMutex);
    IOReturn result = sendStopLocked();
    pthread_mutex_unlock(&ffbMutex);
    char response[96];
    snprintf(response, sizeof(response), "STOP result=%08x\n", (unsigned int)result);
    replyLine(fd, response);
}

static void stopAtExit()
{
    pthread_mutex_lock(&ffbMutex);
    (void)sendStopLocked();
    pthread_mutex_unlock(&ffbMutex);
}


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
                         "STATUS device=DFGT pid=C29A manager_open=%s manager_result=%08x device_ref=%s device_open=%s ffb=%s limit=%d\n",
                         IsHIDManagerOpen() ? "yes" : "no",
                         (unsigned int)HIDManagerOpenResult(),
                         IsNativeDFGTAvailable() ? "retained" : "unavailable",
                         IsNativeDFGTDeviceOpen() ? "yes" : "no",
                         ffbActive ? "active" : "stopped", kMaxForce);
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
            } else if (strncmp(request, "FORCE ", 6) == 0) {
                int force = 0;
                if (!parseForce(request, &force))
                    replyLine(client, "ERROR force_out_of_range_or_invalid (allowed -32..32)\n");
                else
                    handleForce(client, force);
            } else if (strcmp(request, "STOP") == 0) {
                handleStop(client);
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

    pthread_t watchdog;
    result = pthread_create(&watchdog, nullptr, watchdogMain, nullptr);
    if (result != 0) {
        fprintf(stderr, "FFB IPC: watchdog thread failed (%d).\\n", result);
        return false;
    }
    pthread_detach(watchdog);
    atexit(stopAtExit);
    return true;
}

const char *FFBIPCSocketPath()
{
    if (socketPath.empty() && !prepareSocketPath()) return nullptr;
    return socketPath.c_str();
}
