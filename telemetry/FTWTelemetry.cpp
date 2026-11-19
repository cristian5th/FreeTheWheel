#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "amtrucks/scssdk_ats.h"

namespace {

scs_log_t g_log = nullptr;
const char *g_game_name = "SCS";
float g_speed_mps = 0.0f;
float g_steering = 0.0f;
float g_engine_rpm = 0.0f;
unsigned long long g_frames = 0;
int g_ipc_fd = -1;
unsigned int g_sequence = 0;
bool g_driving = false;
bool g_paused = false;
unsigned long long g_last_sample_ms = 0;

unsigned long long monotonic_ms()
{
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<unsigned long long>(ts.tv_sec) * 1000ULL +
           static_cast<unsigned long long>(ts.tv_nsec) / 1000000ULL;
}

void log_message(scs_log_type_t type, const char *message)
{
    if (g_log) {
        g_log(type, message);
    }
}

void log_game_message(scs_log_type_t type, const char *message)
{
    char line[256];
    std::snprintf(line, sizeof(line), "FreeTheWheel %s telemetry: %s",
                  g_game_name, message);
    log_message(type, line);
}

void close_ipc()
{
    if (g_ipc_fd >= 0) {
        close(g_ipc_fd);
        g_ipc_fd = -1;
    }
}

bool send_all_bytes(int fd, const char *data, size_t length,
                    size_t *bytes_sent, int *failure_errno)
{
    size_t sent = 0;
    const unsigned long long deadline = monotonic_ms() + 20ULL;

    while (sent < length) {
        const ssize_t count = send(fd, data + sent, length - sent, 0);
        if (count > 0) {
            sent += static_cast<size_t>(count);
            continue;
        }
        if (count == 0) {
            *failure_errno = EPIPE;
            break;
        }

        const int send_error = errno;
        if (send_error == EINTR) {
            continue;
        }
        if (send_error != EAGAIN && send_error != EWOULDBLOCK) {
            *failure_errno = send_error;
            break;
        }

        const unsigned long long now = monotonic_ms();
        if (now >= deadline) {
            *failure_errno = ETIMEDOUT;
            break;
        }

        pollfd writable {};
        writable.fd = fd;
        writable.events = POLLOUT;
        const int remaining = static_cast<int>(deadline - now);
        const int ready = poll(&writable, 1, remaining);
        if (ready < 0) {
            if (errno == EINTR) continue;
            *failure_errno = errno;
            break;
        }
        if (ready == 0) {
            *failure_errno = ETIMEDOUT;
            break;
        }
        if (writable.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                           &socket_error, &error_length) != 0) {
                socket_error = errno;
            }
            *failure_errno = socket_error != 0 ? socket_error :
                ((writable.revents & POLLNVAL) ? EBADF : EPIPE);
            break;
        }
    }

    *bytes_sent = sent;
    return sent == length;
}

bool send_record(const char *record)
{
    if (g_ipc_fd < 0) {
        return false;
    }

    const size_t length = std::strlen(record);
    size_t bytes_sent = 0;
    int failure_errno = 0;
    if (send_all_bytes(g_ipc_fd, record, length,
                       &bytes_sent, &failure_errno)) {
        return true;
    }

    char message[256];
    std::snprintf(message, sizeof(message),
                  "IPC send failed; disconnecting (errno=%d: %s, sent=%zu/%zu bytes)",
                  failure_errno, std::strerror(failure_errno),
                  bytes_sent, length);
    log_game_message(SCS_LOG_TYPE_warning, message);
    close_ipc();
    return false;
}

bool connect_ipc()
{
    const char *home = std::getenv("HOME");
    if (!home || !*home) {
        log_game_message(SCS_LOG_TYPE_warning, "HOME is unavailable; IPC disabled");
        return false;
    }

    sockaddr_un address {};
    char path[sizeof(address.sun_path)];
    const int path_len = std::snprintf(
        path, sizeof(path), "%s/Library/Application Support/FreeTheWheel/ffb.sock", home);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        log_game_message(SCS_LOG_TYPE_warning, "IPC socket path is too long");
        return false;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_game_message(SCS_LOG_TYPE_warning, "could not create IPC socket");
        return false;
    }

    int no_sigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));

    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        log_game_message(SCS_LOG_TYPE_warning, "FreeTheWheel IPC server unavailable");
        return false;
    }

    timeval timeout {};
    timeout.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char hello[64];
    std::snprintf(hello, sizeof(hello), "HELLO FTW_TELEMETRY 1 %s\n", g_game_name);
    size_t hello_sent = 0;
    int hello_error = 0;
    const size_t hello_length = std::strlen(hello);
    if (!send_all_bytes(fd, hello, hello_length, &hello_sent, &hello_error)) {
        char message[192];
        std::snprintf(message, sizeof(message),
                      "IPC handshake send failed (errno=%d: %s, sent=%zu/%zu bytes)",
                      hello_error, std::strerror(hello_error),
                      hello_sent, hello_length);
        close(fd);
        log_game_message(SCS_LOG_TYPE_warning, message);
        return false;
    }

    char response[128] {};
    const ssize_t count = recv(fd, response, sizeof(response) - 1, 0);
    if (count <= 0) {
        close(fd);
        log_game_message(SCS_LOG_TYPE_warning, "IPC handshake response timed out");
        return false;
    }
    response[count] = '\0';
    if (std::strncmp(response, "READY 1 ", 8) != 0) {
        close(fd);
        log_game_message(SCS_LOG_TYPE_warning, "IPC handshake rejected");
        return false;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        log_game_message(SCS_LOG_TYPE_warning, "could not make IPC socket nonblocking");
        return false;
    }

    g_ipc_fd = fd;
    log_game_message(SCS_LOG_TYPE_message, "connected to FreeTheWheel IPC");
    return true;
}

void send_telemetry(const char *state)
{
    if (g_ipc_fd < 0) {
        return;
    }

    const unsigned long long now = monotonic_ms();
    if (std::strcmp(state, "DRIVING") == 0 &&
        g_last_sample_ms != 0 && now - g_last_sample_ms < 16ULL) {
        return;
    }

    if (!std::isfinite(g_speed_mps) || !std::isfinite(g_steering) ||
        !std::isfinite(g_engine_rpm)) {
        log_game_message(SCS_LOG_TYPE_warning, "non-finite telemetry; closing IPC");
        close_ipc();
        return;
    }

    char record[256];
    ++g_sequence;
    const int length = std::snprintf(
        record, sizeof(record), "T 1 %u %llu %.6f %.6f %.1f %s\n",
        g_sequence, now, g_speed_mps, g_steering, g_engine_rpm, state);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(record)) {
        close_ipc();
        return;
    }
    if (send_record(record) && std::strcmp(state, "DRIVING") == 0) {
        g_last_sample_ms = now;
    }
}

void store_float(const scs_string_t, const scs_u32_t,
                 const scs_value_t *value, const scs_context_t context)
{
    if (!value || value->type != SCS_VALUE_TYPE_float || !context) {
        return;
    }
    *static_cast<float *>(context) = value->value_float.value;
}

SCSAPI_VOID on_event(const scs_event_t event, const void *, const scs_context_t)
{
    if (event == SCS_TELEMETRY_EVENT_paused) {
        g_driving = false;
        g_paused = true;
        send_telemetry("PAUSED");
        log_game_message(SCS_LOG_TYPE_message, "paused; telemetry neutralized");
    } else if (event == SCS_TELEMETRY_EVENT_started) {
        g_paused = false;
        g_driving = true;
        log_game_message(SCS_LOG_TYPE_message, "driving");
    } else if (event == SCS_TELEMETRY_EVENT_frame_end) {
        ++g_frames;
        if (g_driving && !g_paused) {
            send_telemetry("DRIVING");
        }
        if ((g_frames % 300ULL) == 0ULL) {
            char line[192];
            std::snprintf(line, sizeof(line),
                          "frames=%llu speed=%.2f m/s steering=%.3f rpm=%.0f",
                          g_frames, g_speed_mps, g_steering, g_engine_rpm);
            log_game_message(SCS_LOG_TYPE_message, line);
        }
    }
}

} // namespace

extern "C" SCSAPI_RESULT scs_telemetry_init(
    const scs_u32_t version,
    const scs_telemetry_init_params_t *const params)
{
    if (version != SCS_TELEMETRY_VERSION_1_01 || !params) {
        return SCS_RESULT_unsupported;
    }

    const auto *init =
        static_cast<const scs_telemetry_init_params_v101_t *>(params);
    g_log = init->common.log;

    if (!g_log || !init->register_for_event || !init->register_for_channel) {
        return SCS_RESULT_invalid_parameter;
    }

    if (std::strcmp(init->common.game_id, SCS_GAME_ID_EUT2) == 0) {
        g_game_name = "ETS2";
    } else if (std::strcmp(init->common.game_id, SCS_GAME_ID_ATS) == 0) {
        g_game_name = "ATS";
    } else {
        log_message(SCS_LOG_TYPE_warning,
                    "FreeTheWheel telemetry: unsupported game ID");
        g_log = nullptr;
        return SCS_RESULT_unsupported;
    }

    log_game_message(SCS_LOG_TYPE_message, "scs_telemetry_init entered");

    const bool events_ok =
        init->register_for_event(SCS_TELEMETRY_EVENT_paused, on_event, nullptr) == SCS_RESULT_ok &&
        init->register_for_event(SCS_TELEMETRY_EVENT_started, on_event, nullptr) == SCS_RESULT_ok &&
        init->register_for_event(SCS_TELEMETRY_EVENT_frame_end, on_event, nullptr) == SCS_RESULT_ok;

    if (!events_ok) {
        log_game_message(SCS_LOG_TYPE_error, "event registration failed");
        g_log = nullptr;
        return SCS_RESULT_generic_error;
    }

    const scs_u32_t each_frame = SCS_TELEMETRY_CHANNEL_FLAG_each_frame;
    const bool channels_ok =
        init->register_for_channel("truck.speed", SCS_U32_NIL,
            SCS_VALUE_TYPE_float, each_frame, store_float, &g_speed_mps) == SCS_RESULT_ok &&
        init->register_for_channel("truck.effective.steering", SCS_U32_NIL,
            SCS_VALUE_TYPE_float, each_frame, store_float, &g_steering) == SCS_RESULT_ok &&
        init->register_for_channel("truck.engine.rpm", SCS_U32_NIL,
            SCS_VALUE_TYPE_float, each_frame, store_float, &g_engine_rpm) == SCS_RESULT_ok;

    if (!channels_ok) {
        log_game_message(SCS_LOG_TYPE_error, "channel registration failed");
        g_log = nullptr;
        return SCS_RESULT_generic_error;
    }

    g_frames = 0;
    g_speed_mps = 0.0f;
    g_steering = 0.0f;
    g_engine_rpm = 0.0f;
    g_sequence = 0;
    g_driving = false;
    g_paused = false;
    g_last_sample_ms = 0;
    connect_ipc();

    log_game_message(SCS_LOG_TYPE_message,
                     "initialized; waiting for telemetry");
    return SCS_RESULT_ok;
}

extern "C" SCSAPI_VOID scs_telemetry_shutdown(void)
{
    if (g_ipc_fd >= 0) {
        send_record("STOP\n");
        close_ipc();
    }
    log_game_message(SCS_LOG_TYPE_message, "scs_telemetry_shutdown");
    g_log = nullptr;
}
