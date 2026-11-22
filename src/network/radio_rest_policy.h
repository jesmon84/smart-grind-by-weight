#pragma once

#include <cstdint>

// A grind is when the grinder's supply is weakest: the motor is switching and
// the display and processor are at full load. The Wi-Fi radio is the one big
// load nobody needs just then, and its transmit bursts are the sharpest. With
// the rest switched on, the radio is off from the start of a grind until a
// little after it, as on a grinder without Wi-Fi.

// The radio comes back this long after a grind. A second grind within that
// time, or a pulse on the finished screen, finds it still off.
constexpr uint32_t kRadioResumeDelayMs = 15000U;

// Decides when the radio rests.
class RadioRestSchedule {
public:
    // Call often. `grind_active` is true from the start of a grind until its
    // result is on the screen. Returns true while the radio should be off.
    bool update(uint32_t now_ms, bool rest_enabled, bool grind_active) {
        if (!rest_enabled) {
            resting_ = false;
            return false;
        }
        if (grind_active) {
            resting_ = true;
            last_active_ms_ = now_ms;
            return true;
        }
        if (resting_ && now_ms - last_active_ms_ >= kRadioResumeDelayMs) {
            resting_ = false;
        }
        return resting_;
    }

private:
    bool resting_ = false;
    uint32_t last_active_ms_ = 0;
};
