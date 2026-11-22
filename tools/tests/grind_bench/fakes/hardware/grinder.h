#pragma once

#include <Arduino.h>
#include <functional>

#include "../config/constants.h"
#include "../controllers/grind_events.h"

// Bench motor: remembers what the controller asked for, with the timing rules
// of the real driver (src/hardware/grinder.cpp). The bench turns "motor on"
// into grounds.
class Grinder {
public:
    bool grinding = false;
    bool pulse_active = false;
    unsigned long motor_start_time = 0;
    unsigned long pulse_end_ms = 0;
    int continuous_starts = 0;
    int pulses = 0;

    void start() {
        stop();
        motor_start_time = millis();
        grinding = true;
        ++continuous_starts;
    }

    void stop() {
        grinding = false;
        pulse_active = false;
    }

    void start_pulse_rmt(uint32_t duration_ms) {
        stop();
        motor_start_time = millis();
        pulse_end_ms = motor_start_time + duration_ms;
        pulse_active = true;
        grinding = true;
        ++pulses;
    }

    bool is_pulse_complete() {
        if (!pulse_active) return true;
        if (millis() < pulse_end_ms) return false;
        pulse_active = false;
        grinding = false;
        return true;
    }

    // The output is high for the whole of a continuous run, but only for the
    // requested time of a pulse, whenever the controller next looks.
    bool output_high() const { return grinding && (!pulse_active || millis() < pulse_end_ms); }

    bool is_grinding() const { return grinding; }
    bool is_initialized() const { return true; }
    bool is_motor_settled() const {
        if (motor_start_time == 0) return false;
        return (millis() - motor_start_time) >= HW_GRINDER_SETTLING_TIME_MS;
    }
    void set_ui_event_callback(const std::function<void(const GrindEventData&)>&) {}
};
