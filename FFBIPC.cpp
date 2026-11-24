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
#include <cmath>
#include <algorithm>
#include <chrono>
#include <vector>
#include <new>
#include <sys/time.h>

static std::string socketPath;
static pthread_mutex_t ffbMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t telemetryMutex = PTHREAD_MUTEX_INITIALIZER;

static bool ffbSessionOpen = false;
static bool ffbActive = false;
static bool telemetryOutputActive = false;
static bool telemetryFFBEnabled = false;
static double filteredSteering = 0.0, filteredForce = 0.0, previousSteering = 0.0;
static bool telemetryFilterInitialized = false;
static std::chrono::steady_clock::time_point previousTelemetryTime{};
static time_t lastForceTime = 0;
static constexpr int kMaxForce = 32;
static constexpr int kMaxAutoForce = 16;
static constexpr time_t kWatchdogSeconds = 2;

static constexpr size_t kMaxLineLength = 255;
static constexpr int kTelemetryTimeoutMs = 300;
static constexpr int kTelemetryMaxRateHz = 100;

struct TelemetrySample {
    uint32_t sequence = 0;
    uint64_t senderMonotonicMs = 0;
    double speedMps = 0.0;
    double steering = 0.0;
    double rpm = 0.0;
    bool driving = false;
    std::chrono::steady_clock::time_point receivedAt{};
};

static int telemetryClientFD = -1;
static bool telemetrySampleValid = false;
static TelemetrySample latestTelemetrySample;
static std::chrono::steady_clock::time_point lastTelemetryAccepted{};
static bool telemetrySequenceValid = false;
static uint32_t lastTelemetrySequence = 0;

static void replyLine(int fd, const char *line);

static IOReturn sendStopLocked()
{
    if (!ffbSessionOpen) {
        ffbActive = false;
        telemetryOutputActive = false;
        return kIOReturnSuccess;
    }

    const UInt8 report[7] = {0x13, 0, 0, 0, 0, 0, 0};
    IOReturn result = SendNativeDFGTOutputReport(report, 7);
    ffbActive = false;
    telemetryOutputActive = false;
    IOReturn closeResult = CloseNativeDFGTDevice();
    ffbSessionOpen = false;
    return result != kIOReturnSuccess ? result : closeResult;
}

static void resetTelemetryFilterLocked()
{
    filteredSteering = filteredForce = previousSteering = 0.0;
    telemetryFilterInitialized = false;
    previousTelemetryTime = std::chrono::steady_clock::time_point{};
}

static IOReturn neutralizeTelemetryEffectLocked()
{
    if (!ffbSessionOpen) {
        ffbActive = telemetryOutputActive = false;
        return kIOReturnSuccess;
    }
    const UInt8 report[7] = {0x13, 0, 0, 0, 0, 0, 0};
    const IOReturn result = SendNativeDFGTOutputReport(report, 7);
    ffbActive = telemetryOutputActive = false;
    return result;
}

static void applyTelemetryForce(const TelemetrySample &sample)
{
    static auto diagnosticWindowStart = std::chrono::steady_clock::time_point{};
    static uint64_t diagnosticSamples = 0;
    static uint64_t diagnosticZeroSamples = 0;
    static uint64_t diagnosticForceReports = 0;
    static uint64_t diagnosticForceSuccesses = 0;
    static uint64_t diagnosticNeutralReports = 0;
    static IOReturn diagnosticLastResult = kIOReturnSuccess;

    pthread_mutex_lock(&ffbMutex);
    if (!telemetryFFBEnabled || !sample.driving ||
        std::fabs(sample.speedMps) < 0.5) {
        if (telemetryOutputActive &&
            neutralizeTelemetryEffectLocked() != kIOReturnSuccess)
            (void)sendStopLocked();
        resetTelemetryFilterLocked();
        pthread_mutex_unlock(&ffbMutex);
        return;
    }

    const auto now = sample.receivedAt;
    double dt = 1.0 / 60.0;
    if (telemetryFilterInitialized) {
        dt = std::chrono::duration<double>(now - previousTelemetryTime).count();
        dt = std::clamp(dt, 0.001, 0.100);
    }

    const double steering = std::clamp(sample.steering, -1.0, 1.0);
    const double alpha = dt / (0.040 + dt);
    if (!telemetryFilterInitialized) {
        filteredSteering = previousSteering = steering;
        filteredForce = 0.0;
        telemetryFilterInitialized = true;
    } else {
        filteredSteering += alpha * (steering - filteredSteering);
    }

    const double steeringRate = std::clamp(
        (filteredSteering - previousSteering) / dt, -4.0, 4.0);
    previousSteering = filteredSteering;
    previousTelemetryTime = now;

    const double speedGain = std::clamp(std::fabs(sample.speedMps) / 8.0, 0.15, 1.0);
    // Double the automatic centering response and raise its output ceiling
    // progressively, while retaining filtering, damping and slew limiting.
    const double centering = std::fabs(filteredSteering) < 0.015
        ? 0.0 : -0.64 * filteredSteering;
    const double target = std::clamp(
        speedGain * (centering - 0.035 * steeringRate), -0.50, 0.50);

    const double maxStep = dt; // 1.0 normalized unit per second.
    const double slewed = filteredForce +
        std::clamp(target - filteredForce, -maxStep, maxStep);
    filteredForce += alpha * (slewed - filteredForce);

    // Same constant-force report as the verified manual test; auto output <= FORCE 16.
    const int force = std::clamp(
        static_cast<int>(std::lround(filteredForce * 32.0)),
        -kMaxAutoForce, kMaxAutoForce);

    ++diagnosticSamples;
    if (force == 0) ++diagnosticZeroSamples;

    IOReturn result = kIOReturnSuccess;
    if (force == 0) {
        if (telemetryOutputActive) {
            result = neutralizeTelemetryEffectLocked();
            ++diagnosticNeutralReports;
            diagnosticLastResult = result;
        }
    } else {
        if (!IsNativeDFGTAvailable()) {
            result = kIOReturnNoDevice;
        } else if (!ffbSessionOpen) {
            result = OpenNativeDFGTDeviceShared();
            if (result == kIOReturnSuccess) ffbSessionOpen = true;
        }
        if (result == kIOReturnSuccess) {
            const UInt8 report[7] = {
                0x11, 0x08, static_cast<UInt8>(0x80 + force), 0x80, 0, 0, 0
            };
            ++diagnosticForceReports;
            result = SendNativeDFGTOutputReport(report, 7);
            diagnosticLastResult = result;
            if (result == kIOReturnSuccess) {
                ++diagnosticForceSuccesses;
                ffbActive = telemetryOutputActive = true;
            }
        }
    }

    diagnosticLastResult = result;
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "FFB IPC: automatic output failed (%08x); disabling.\n",
                static_cast<unsigned int>(result));
        (void)sendStopLocked();
        telemetryFFBEnabled = false;
        resetTelemetryFilterLocked();
    }

    const auto diagnosticNow = std::chrono::steady_clock::now();
    if (diagnosticWindowStart == std::chrono::steady_clock::time_point{})
        diagnosticWindowStart = diagnosticNow;
    if (std::chrono::duration<double>(diagnosticNow - diagnosticWindowStart).count() >= 1.0) {
        fprintf(stderr,
                "FFB AUTO DIAG: speed=%.2f steer_in=%.4f steer_filt=%.4f "
                "steer_rate=%.4f target=%.4f force_filt=%.4f force_cmd=%d "
                "samples=%llu zero=%llu reports=%llu reports_ok=%llu neutral=%llu "
                "last_result=%08x\n",
                sample.speedMps, sample.steering, filteredSteering, steeringRate,
                target, filteredForce, force,
                static_cast<unsigned long long>(diagnosticSamples),
                static_cast<unsigned long long>(diagnosticZeroSamples),
                static_cast<unsigned long long>(diagnosticForceReports),
                static_cast<unsigned long long>(diagnosticForceSuccesses),
                static_cast<unsigned long long>(diagnosticNeutralReports),
                static_cast<unsigned int>(diagnosticLastResult));
        diagnosticSamples = diagnosticZeroSamples = 0;
        diagnosticForceReports = diagnosticForceSuccesses = 0;
        diagnosticNeutralReports = 0;
        diagnosticLastResult = kIOReturnSuccess;
        diagnosticWindowStart = diagnosticNow;
    }
    pthread_mutex_unlock(&ffbMutex);
}

static void *watchdogMain(void *)
{
    for (;;) {
        sleep(1);
        pthread_mutex_lock(&ffbMutex);
        if (ffbSessionOpen && ffbActive && !telemetryOutputActive &&
            time(nullptr) - lastForceTime >= kWatchdogSeconds) {
            (void)sendStopLocked();
        }
        pthread_mutex_unlock(&ffbMutex);
    }
    return nullptr;
}

static bool parseForce(const char *request, int *force)
{
    if (strncmp(request, "FORCE ", 6) != 0) return false;

    char *end = nullptr;
    errno = 0;
    long value = strtol(request + 6, &end, 10);
    if (errno != 0 || end == request + 6 || *end != '\0' ||
        value < -kMaxForce || value > kMaxForce) {
        return false;
    }

    *force = static_cast<int>(value);
    return true;
}

static void handleForce(int fd, int force)
{
    pthread_mutex_lock(&ffbMutex);
    telemetryFFBEnabled = false;
    resetTelemetryFilterLocked();

    if (!IsNativeDFGTAvailable()) {
        replyLine(fd, "ERROR no_device\n");
    } else if (force == 0) {
        IOReturn stopResult = sendStopLocked();
        char response[96];
        snprintf(response, sizeof(response),
                 "FORCE neutral stop_result=%08x\n",
                 static_cast<unsigned int>(stopResult));
        replyLine(fd, response);
    } else {
        IOReturn result = kIOReturnSuccess;

        if (!ffbSessionOpen) {
            result = OpenNativeDFGTDeviceShared();
            if (result == kIOReturnSuccess) ffbSessionOpen = true;
        }

        if (result == kIOReturnSuccess) {
            const UInt8 report[7] = {
                0x11, 0x08, static_cast<UInt8>(0x80 + force), 0x80, 0, 0, 0
            };
            result = SendNativeDFGTOutputReport(report, 7);
        }

        if (result == kIOReturnSuccess) {
            ffbActive = true;
            lastForceTime = time(nullptr);
            char response[96];
            snprintf(response, sizeof(response),
                     "FORCE accepted value=%d limit=%d\n", force, kMaxForce);
            replyLine(fd, response);
        } else {
            (void)sendStopLocked();
            char response[96];
            snprintf(response, sizeof(response),
                     "ERROR force_io_result=%08x\n",
                     static_cast<unsigned int>(result));
            replyLine(fd, response);
        }
    }

    pthread_mutex_unlock(&ffbMutex);
}

static void handleStop(int fd)
{
    pthread_mutex_lock(&ffbMutex);
    telemetryFFBEnabled = false;
    resetTelemetryFilterLocked();
    IOReturn result = sendStopLocked();
    pthread_mutex_unlock(&ffbMutex);

    char response[96];
    snprintf(response, sizeof(response), "STOP result=%08x\n",
             static_cast<unsigned int>(result));
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

    std::string dir = std::string(home) +
        "/Library/Application Support/FreeTheWheel";

    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;

    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid()) {
        return false;
    }

    if (chmod(dir.c_str(), 0700) != 0) return false;

    socketPath = dir + "/ffb.sock";
    return socketPath.size() < sizeof(((sockaddr_un *)0)->sun_path);
}

static void replyLine(int fd, const char *line)
{
    if (fd < 0 || !line) return;

    const size_t length = strlen(line);
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, line + sent, length - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static bool parseUnsigned(const char *text, uint64_t maxValue, uint64_t *value)
{
    if (!text || !*text || *text < '0' || *text > '9') return false;

    errno = 0;
    char *end = nullptr;
    unsigned long long parsed = strtoull(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' ||
        parsed > maxValue) {
        return false;
    }

    *value = static_cast<uint64_t>(parsed);
    return true;
}

static bool parseFiniteDouble(const char *text, double minValue,
                              double maxValue, double *value)
{
    if (!text || !*text) return false;

    errno = 0;
    char *end = nullptr;
    double parsed = strtod(text, &end);

    if (errno != 0 || end == text || *end != '\0' ||
        !std::isfinite(parsed) || parsed < minValue || parsed > maxValue) {
        return false;
    }

    *value = parsed;
    return true;
}

static std::vector<std::string> splitFields(const char *line)
{
    std::vector<std::string> fields;
    const char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        fields.emplace_back(start, static_cast<size_t>(p - start));
    }

    return fields;
}

static bool parseHello(const char *line, const char **game)
{
    const auto fields = splitFields(line);
    if (fields.size() != 4 || fields[0] != "HELLO" ||
        fields[1] != "FTW_TELEMETRY" || fields[2] != "1") {
        return false;
    }

    if (fields[3] == "ETS2") {
        *game = "ETS2";
        return true;
    }
    if (fields[3] == "ATS") {
        *game = "ATS";
        return true;
    }
    return false;
}

static bool parseTelemetry(const char *line, TelemetrySample *sample)
{
    const auto fields = splitFields(line);
    if (fields.size() != 8 || fields[0] != "T" || fields[1] != "1")
        return false;

    uint64_t sequence = 0;
    uint64_t senderTime = 0;

    if (!parseUnsigned(fields[2].c_str(), UINT32_MAX, &sequence) ||
        !parseUnsigned(fields[3].c_str(), UINT64_MAX, &senderTime) ||
        !parseFiniteDouble(fields[4].c_str(), -100.0, 100.0,
                           &sample->speedMps) ||
        !parseFiniteDouble(fields[5].c_str(), -1.0, 1.0,
                           &sample->steering) ||
        !parseFiniteDouble(fields[6].c_str(), 0.0, 20000.0,
                           &sample->rpm)) {
        return false;
    }

    if (fields[7] != "DRIVING" && fields[7] != "PAUSED" &&
        fields[7] != "STOPPED") {
        return false;
    }

    sample->sequence = static_cast<uint32_t>(sequence);
    sample->senderMonotonicMs = senderTime;
    sample->driving = fields[7] == "DRIVING";
    sample->receivedAt = std::chrono::steady_clock::now();
    return true;
}

static bool isCurrentTelemetryClient(int fd)
{
    pthread_mutex_lock(&telemetryMutex);
    const bool current = telemetryClientFD == fd;
    pthread_mutex_unlock(&telemetryMutex);
    return current;
}

static IOReturn stopTelemetryOutput()
{
    pthread_mutex_lock(&ffbMutex);
    IOReturn result = sendStopLocked();
    pthread_mutex_unlock(&ffbMutex);
    return result;
}

static void endTelemetrySession(int fd)
{
    bool wasCurrent = false;

    pthread_mutex_lock(&telemetryMutex);
    if (telemetryClientFD == fd) {
        telemetryClientFD = -1;
        telemetrySampleValid = false;
        telemetrySequenceValid = false;
        wasCurrent = true;
    }
    pthread_mutex_unlock(&telemetryMutex);

    if (wasCurrent) {
        fprintf(stderr, "FFB IPC: telemetry session ended; output neutralized.\n");
        stopTelemetryOutput();
    }
}

static bool acceptTelemetrySample(int fd, const char *line)
{
    TelemetrySample sample;
    if (!parseTelemetry(line, &sample)) return false;

    pthread_mutex_lock(&telemetryMutex);

    if (telemetryClientFD != fd) {
        pthread_mutex_unlock(&telemetryMutex);
        return false;
    }

    if (telemetrySequenceValid) {
        const uint32_t delta = sample.sequence - lastTelemetrySequence;
        if (delta == 0 || delta >= 0x80000000U) {
            pthread_mutex_unlock(&telemetryMutex);
            return false;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (sample.driving && telemetrySampleValid) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - lastTelemetryAccepted).count();
        if (elapsed < 1000000 / kTelemetryMaxRateHz) {
            pthread_mutex_unlock(&telemetryMutex);
            return true; // Rate-limited driving sample: ignore, do not queue.
        }
    }

    lastTelemetrySequence = sample.sequence;
    telemetrySequenceValid = true;
    lastTelemetryAccepted = now;
    latestTelemetrySample = sample;
    telemetrySampleValid = sample.driving;

    const bool shouldStop = !sample.driving;
    pthread_mutex_unlock(&telemetryMutex);

    if (shouldStop) {
        stopTelemetryOutput();
        pthread_mutex_lock(&ffbMutex);
        resetTelemetryFilterLocked();
        pthread_mutex_unlock(&ffbMutex);
    } else {
        applyTelemetryForce(sample);
    }
    return true;
}

static bool registerTelemetryClient(int fd)
{
    int previous = -1;

    pthread_mutex_lock(&telemetryMutex);
    previous = telemetryClientFD;
    telemetryClientFD = fd;
    telemetrySampleValid = false;
    telemetrySequenceValid = false;
    lastTelemetryAccepted = std::chrono::steady_clock::now();
    pthread_mutex_unlock(&telemetryMutex);

    if (previous >= 0 && previous != fd) {
        shutdown(previous, SHUT_RDWR);
    }

    stopTelemetryOutput();
    return true;
}

static bool handleLegacyCommand(int fd, const char *request)
{
    if (strcmp(request, "PING") == 0) {
        replyLine(fd, "PONG protocol=1\n");
    } else if (strcmp(request, "STATUS") == 0) {
        char status[320];
        pthread_mutex_lock(&ffbMutex);
        const bool deviceAvailable = IsNativeDFGTAvailable();
        const bool deviceOpen = IsNativeDFGTDeviceOpen();
        const bool active = ffbActive;
        snprintf(status, sizeof(status),
                 "STATUS device=DFGT pid=C29A manager_open=%s manager_result=%08x device_ref=%s device_open=%s ffb=%s limit=%d auto_ffb=%s\n",
                 IsHIDManagerOpen() ? "yes" : "no",
                 static_cast<unsigned int>(HIDManagerOpenResult()),
                 deviceAvailable ? "retained" : "unavailable",
                 deviceOpen ? "yes" : "no",
                 active ? "active" : "stopped", kMaxForce,
                 telemetryFFBEnabled ? "enabled" : "disabled");
        pthread_mutex_unlock(&ffbMutex);

        pthread_mutex_lock(&telemetryMutex);
        const bool connected = telemetryClientFD >= 0;
        const bool sampleValid = telemetrySampleValid;
        pthread_mutex_unlock(&telemetryMutex);

        char *marker = strstr(status, "\n");
        if (marker) {
            *marker = '\0';
            char extended[384];
            snprintf(extended, sizeof(extended),
                     "%s telemetry=%s sample=%s\n",
                     status, connected ? "connected" : "disconnected",
                     sampleValid ? "fresh" : "none");
            replyLine(fd, extended);
        } else {
            replyLine(fd, status);
        }
    } else if (strcmp(request, "SHARED_PROBE") == 0) {
        pthread_mutex_lock(&ffbMutex);
        IOReturn openResult = OpenNativeDFGTDeviceShared();
        IOReturn closeResult = kIOReturnNotReady;

        if (openResult == kIOReturnSuccess)
            closeResult = CloseNativeDFGTDevice();

        char response[160];
        snprintf(response, sizeof(response),
                 "SHARED_PROBE open=%s open_result=%08x close=%s close_result=%08x device_open=%s\n",
                 openResult == kIOReturnSuccess ? "success" : "failed",
                 static_cast<unsigned int>(openResult),
                 closeResult == kIOReturnSuccess ? "success" : "failed",
                 static_cast<unsigned int>(closeResult),
                 IsNativeDFGTDeviceOpen() ? "yes" : "no");
        pthread_mutex_unlock(&ffbMutex);
        replyLine(fd, response);
    } else if (strcmp(request, "PROBE") == 0) {
        pthread_mutex_lock(&ffbMutex);
        if (!IsNativeDFGTAvailable()) {
            pthread_mutex_unlock(&ffbMutex);
            replyLine(fd, "PROBE open=not-attempted result=no_device device_open=no\n");
        } else {
            IOReturn openResult = OpenNativeDFGTDevice();
            if (openResult != kIOReturnSuccess) {
                char response[128];
                snprintf(response, sizeof(response),
                         "PROBE open=failed result=%08x device_open=%s\n",
                         static_cast<unsigned int>(openResult),
                         IsNativeDFGTDeviceOpen() ? "yes" : "no");
                pthread_mutex_unlock(&ffbMutex);
                replyLine(fd, response);
            } else {
                IOReturn closeResult = CloseNativeDFGTDevice();
                char response[160];
                snprintf(response, sizeof(response),
                         "PROBE open=success close=%s close_result=%08x device_open=%s\n",
                         closeResult == kIOReturnSuccess ? "success" : "failed",
                         static_cast<unsigned int>(closeResult),
                         IsNativeDFGTDeviceOpen() ? "yes" : "no");
                pthread_mutex_unlock(&ffbMutex);
                replyLine(fd, response);
            }
        }
    } else if (strncmp(request, "FORCE ", 6) == 0) {
        int force = 0;
        if (!parseForce(request, &force)) {
            replyLine(fd, "ERROR force_out_of_range_or_invalid (allowed -32..32)\n");
        } else {
            handleForce(fd, force);
        }
    } else if (strcmp(request, "AUTO_FFB ON") == 0 ||
               strcmp(request, "AUTO_FFB OFF") == 0) {
        const bool enable = strcmp(request, "AUTO_FFB ON") == 0;
        pthread_mutex_lock(&ffbMutex);
        telemetryFFBEnabled = enable;
        resetTelemetryFilterLocked();
        IOReturn result = enable ? kIOReturnSuccess : sendStopLocked();
        pthread_mutex_unlock(&ffbMutex);
        char response[128];
        snprintf(response, sizeof(response), "AUTO_FFB enabled=%s result=%08x\n",
                 enable ? "yes" : "no", static_cast<unsigned int>(result));
        replyLine(fd, response);
    } else if (strcmp(request, "STOP") == 0) {
        handleStop(fd);
    } else {
        replyLine(fd, "ERROR unknown_command\n");
    }

    return true;
}

struct ClientContext {
    int fd;
};

static void *clientMain(void *argument)
{
    ClientContext *context = static_cast<ClientContext *>(argument);
    const int fd = context->fd;
    delete context;

    int noSignal = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char line[kMaxLineLength + 1]{};
    size_t used = 0;
    bool telemetryMode = false;
    bool firstLine = true;
    bool done = false;

    while (!done) {
        char input[512];
        ssize_t count = recv(fd, input, sizeof(input), 0);

        if (count == 0) {
            if (telemetryMode) {
                fprintf(stderr,
                        "FFB IPC: telemetry peer closed connection (fd=%d).\n", fd);
            }
            // Legacy one-shot commands historically did not require a newline.
            // Telemetry sessions always use newline-delimited records.
            if (firstLine && !telemetryMode && used > 0) {
                line[used] = '\0';
                handleLegacyCommand(fd, line);
            }
            break;
        }

        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (telemetryMode) {
                    if (!isCurrentTelemetryClient(fd)) break;

                    pthread_mutex_lock(&telemetryMutex);
                    // The handshake may happen while the game is still loading or
                    // sitting in its menus. Enforce the watchdog only after a
                    // DRIVING sample has made telemetrySampleValid true. PAUSED
                    // and STOPPED samples clear it and neutralize force output.
                    const bool stale = telemetryClientFD == fd &&
                        telemetrySampleValid &&
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() -
                            lastTelemetryAccepted).count() > kTelemetryTimeoutMs;
                    pthread_mutex_unlock(&telemetryMutex);

                    if (stale) {
                        fprintf(stderr, "FFB IPC: telemetry timeout (> %d ms).\n",
                                kTelemetryTimeoutMs);
                        break;
                    }
                }
                continue;
            }
            break;
        }

        for (ssize_t i = 0; i < count && !done; ++i) {
            const char ch = input[i];

            if (ch == '\n') {
                if (used > 0 && line[used - 1] == '\r') --used;
                line[used] = '\0';

                if (firstLine) {
                    firstLine = false;
                    const char *game = nullptr;

                    if (parseHello(line, &game)) {
                        telemetryMode = true;
                        registerTelemetryClient(fd);
                        char ready[64];
                        snprintf(ready, sizeof(ready),
                                 "READY 1 %d\n", kTelemetryMaxRateHz);
                        replyLine(fd, ready);
                        fprintf(stderr, "FFB IPC: persistent telemetry session (%s).\n",
                                game);
                    } else {
                        handleLegacyCommand(fd, line);
                        done = true;
                    }
                } else if (telemetryMode) {
                    if (strcmp(line, "STOP") == 0) {
                        IOReturn stopResult = stopTelemetryOutput();
                        char response[96];
                        snprintf(response, sizeof(response), "STOP result=%08x\n",
                                 static_cast<unsigned int>(stopResult));
                        replyLine(fd, response);
                        done = true;
                    } else if (!acceptTelemetrySample(fd, line)) {
                        fprintf(stderr,
                                "FFB IPC: rejected telemetry sample (fd=%d): %s\n",
                                fd, line);
                        replyLine(fd, "ERR invalid_or_stale_sample\n");
                        done = true;
                    }
                } else {
                    done = true;
                }

                used = 0;
            } else {
                if (used >= kMaxLineLength) {
                    replyLine(fd, "ERR line_too_long\n");
                    done = true;
                } else if (ch == '\0' || (static_cast<unsigned char>(ch) < 0x20 &&
                                         ch != '\t')) {
                    replyLine(fd, "ERR invalid_character\n");
                    done = true;
                } else {
                    line[used++] = ch;
                }
            }
        }

        // Preserve the original one-shot client behavior: legacy commands may
        // be sent without a newline or a write-side shutdown.
        if (!done && firstLine && !telemetryMode && used > 0) {
            line[used] = '\0';
            int parsedForce = 0;
            const bool completeLegacy =
                strcmp(line, "PING") == 0 ||
                strcmp(line, "STATUS") == 0 ||
                strcmp(line, "AUTO_FFB ON") == 0 ||
                strcmp(line, "AUTO_FFB OFF") == 0 ||
                strcmp(line, "SHARED_PROBE") == 0 ||
                strcmp(line, "PROBE") == 0 ||
                strcmp(line, "STOP") == 0 ||
                parseForce(line, &parsedForce);
            if (completeLegacy) {
                handleLegacyCommand(fd, line);
                done = true;
            }
        }
    }

    if (telemetryMode) endTelemetrySession(fd);
    close(fd);
    return nullptr;
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
        chmod(address.sun_path, 0600) != 0 || listen(server, 8) != 0) {
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

        int noSignal = 1;
        (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE,
                         &noSignal, sizeof(noSignal));

        uid_t peerUID = static_cast<uid_t>(-1);
        gid_t peerGID = static_cast<gid_t>(-1);
        if (getpeereid(client, &peerUID, &peerGID) != 0 ||
            peerUID != getuid()) {
            replyLine(client, "ERROR unauthorized\n");
            close(client);
            continue;
        }

        ClientContext *context = new (std::nothrow) ClientContext{client};
        if (!context) {
            replyLine(client, "ERROR server_busy\n");
            close(client);
            continue;
        }

        pthread_t worker;
        const int result = pthread_create(&worker, nullptr, clientMain, context);
        if (result != 0) {
            delete context;
            replyLine(client, "ERROR server_busy\n");
            close(client);
            continue;
        }
        pthread_detach(worker);
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
        fprintf(stderr, "FFB IPC: watchdog thread failed (%d).\n", result);
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
