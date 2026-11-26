#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// When a state message goes out to the connected web and Home Assistant
// clients. Every message wakes the Wi-Fi radio, so an idle grinder only sends
// one when what a client shows has changed, and a keep-alive now and then.

// What a client is shown.
struct LiveState {
    const char* phase = "IDLE";
    bool active = false;
    uint8_t mode = 0;
    int profile = 0;
    int progress = 0;
    float target_weight = 0.0f;
    uint32_t target_time_ms = 0;
    float weight = 0.0f;
    float flow = 0.0f;
    bool motor_running = false;
};

// A resting scale wanders by a few hundredths of a gram. Smaller changes than
// these are not worth a message of their own; the keep-alive carries them.
constexpr float kLiveWeightStepG = 0.05f;
constexpr float kLiveFlowStepGPerS = 0.05f;

// True when a client showing `sent` would show something else for `now`.
inline bool live_state_changed(const LiveState& sent, const LiveState& now) {
    return std::strcmp(sent.phase, now.phase) != 0 ||
           sent.active != now.active ||
           sent.mode != now.mode ||
           sent.profile != now.profile ||
           sent.progress != now.progress ||
           sent.target_weight != now.target_weight ||
           sent.target_time_ms != now.target_time_ms ||
           sent.motor_running != now.motor_running ||
           std::fabs(sent.weight - now.weight) >= kLiveWeightStepG ||
           std::fabs(sent.flow - now.flow) >= kLiveFlowStepGPerS;
}

// During a grind a message goes out this often.
constexpr uint32_t kActiveStateIntervalMs = 100U;
// While idle, changes go out no closer together than this...
constexpr uint32_t kIdleStateIntervalMs = 500U;
// ...and without a change a message still goes out this often, so a client
// can tell a quiet grinder from a lost connection and picks up the small
// changes that got no message of their own. The Home Assistant integration
// takes an idle update at most every two seconds, so the keep-alive comes a
// little less often than that and is never dropped there.
constexpr uint32_t kIdleKeepaliveIntervalMs = 2500U;

// True when a state message should go out. `since_last_ms` is the time since
// the last one. A grind in progress and a change of phase are sent at once.
constexpr bool state_message_due(uint32_t since_last_ms, bool grind_in_progress,
                                 bool phase_changed, bool state_changed) {
    if (grind_in_progress || phase_changed) {
        return true;
    }
    if (since_last_ms < kIdleStateIntervalMs) {
        return false;
    }
    return state_changed || since_last_ms >= kIdleKeepaliveIntervalMs;
}
