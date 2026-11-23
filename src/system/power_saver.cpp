#include "power_saver.h"

#include <Arduino.h>

#include "../config/constants.h"

PowerSaver power_saver;

namespace {
// The Bluetooth update handler sets the clock as well. The clock is read back
// this often, so the two cannot stay at odds.
constexpr uint32_t kClockCheckIntervalMs = 1000U;
}

void PowerSaver::update(bool display_in_use, bool grind_active, bool bluetooth_busy, bool receiving_firmware) {
    display_in_use_.store(display_in_use);

    const uint32_t wanted_mhz = cpu_clock_mhz(display_in_use, grind_active, bluetooth_busy, receiving_firmware);
    const uint32_t now_ms = millis();
    const bool newly_wanted = wanted_mhz != wanted_mhz_;
    wanted_mhz_ = wanted_mhz;
    if (!newly_wanted && now_ms - last_clock_check_ms_ < kClockCheckIntervalMs) {
        return;
    }
    last_clock_check_ms_ = now_ms;
    apply_clock(wanted_mhz);
}

void PowerSaver::display_waking() {
    display_in_use_.store(true);
    wanted_mhz_ = kCpuMhzInUse;
    last_clock_check_ms_ = millis();
    apply_clock(kCpuMhzInUse);
}

void PowerSaver::apply_clock(uint32_t wanted_mhz) {
    if (getCpuFrequencyMhz() != wanted_mhz) {
        if (setCpuFrequencyMhz(wanted_mhz)) {
            LOG_BLE("[POWER] Processor clock %lu MHz\n", static_cast<unsigned long>(wanted_mhz));
        } else {
            LOG_BLE("[POWER] Processor clock could not be set to %lu MHz\n",
                    static_cast<unsigned long>(wanted_mhz));
        }
    }
    cpu_mhz_.store(getCpuFrequencyMhz());
}
