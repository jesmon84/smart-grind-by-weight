#pragma once

#include "grind_events.h"

// Words for the error screen when a grind runs into its time limit. The
// phase it stopped in goes to the diagnostic log; the screen says what that
// means for the person at the grinder.
struct GrindTimeoutText {
    const char* title;    // nullptr keeps the default "ERROR" heading
    const char* message;
};

constexpr GrindTimeoutText grind_timeout_text(GrindMode mode, GrindPhase phase) {
    if (mode == GrindMode::MANUAL) {
        return {nullptr, "30s safety limit"};
    }
    if (mode == GrindMode::TIME) {
        return {"TIMEOUT", "Time limit reached"};
    }
    switch (phase) {
        case GrindPhase::INITIALIZING:
        case GrindPhase::SETUP:
        case GrindPhase::TARING:
        case GrindPhase::TARE_CONFIRM:
            // The motor never started for the dose.
            return {"TIMEOUT", "Could not tare"};
        default:
            return {"TIMEOUT", "Target not reached"};
    }
}
