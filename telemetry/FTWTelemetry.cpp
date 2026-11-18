#include <cstdio>
#include <cstring>

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

void log_message(scs_log_type_t type, const char *message)
{
    if (g_log) {
        g_log(type, message);
    }
}

void log_game_message(scs_log_type_t type, const char *message)
{
    char line[256];
    std::snprintf(line, sizeof(line), "FreeTheWheel %s telemetry probe: %s",
                  g_game_name, message);
    log_message(type, line);
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
        log_game_message(SCS_LOG_TYPE_message, "paused");
    } else if (event == SCS_TELEMETRY_EVENT_started) {
        log_game_message(SCS_LOG_TYPE_message, "driving");
    } else if (event == SCS_TELEMETRY_EVENT_frame_end) {
        ++g_frames;
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
                    "FreeTheWheel telemetry probe: unsupported game ID");
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

    log_game_message(SCS_LOG_TYPE_message,
                     "initialized; waiting for telemetry");
    return SCS_RESULT_ok;
}

extern "C" SCSAPI_VOID scs_telemetry_shutdown(void)
{
    log_game_message(SCS_LOG_TYPE_message, "scs_telemetry_shutdown");
    g_log = nullptr;
}
