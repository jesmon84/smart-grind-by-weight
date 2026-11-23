#pragma once

#include <atomic>
#include <cstdint>

#include "power_saving_policy.h"

// Applies the rules in power_saving_policy.h: follows whether the display is
// in use and runs the processor at the clock that fits.
class PowerSaver {
public:
    // Call from the UI task every cycle. `display_in_use`: the display is on
    // at its normal brightness, neither dimmed nor off. `receiving_firmware`:
    // firmware data is arriving over Bluetooth.
    void update(bool display_in_use, bool grind_active, bool bluetooth_busy, bool receiving_firmware = false);
    // Call from the UI task when the display is about to be switched on. The
    // processor goes to full speed at once, so the screen is drawn quickly
    // without waiting for the next update().
    void display_waking();

    // For other tasks.
    bool display_resting() const { return !display_in_use_.load(); }
    uint32_t cpu_mhz() const { return cpu_mhz_.load(); }

private:
    std::atomic<bool> display_in_use_{true};
    std::atomic<uint32_t> cpu_mhz_{kCpuMhzInUse};
    uint32_t wanted_mhz_ = kCpuMhzInUse;
    uint32_t last_clock_check_ms_ = 0;

    void apply_clock(uint32_t wanted_mhz);
};

extern PowerSaver power_saver;
