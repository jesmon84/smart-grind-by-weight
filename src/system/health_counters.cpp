#include "health_counters.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>
#include <esp_attr.h>
#include <esp_system.h>

#include "../config/constants.h"

namespace {
constexpr uint32_t kFirstSaveAfterStartMs = 30000;
constexpr uint32_t kSaveRetryMs = 10000;
constexpr char kNamespace[] = "health";
constexpr char kBrownoutsKey[] = "brownouts";
constexpr char kPanelFaultsKey[] = "panel_faults";
constexpr char kPanelRecoveriesKey[] = "panel_fixed";
constexpr char kLastBrownoutKey[] = "bo_last";

// The latest RunSnapshot, as bytes so that no constructor clears it at start.
// It survives a restart but not a power cycle.
RTC_NOINIT_ATTR uint8_t rtc_run_snapshot[sizeof(RunSnapshot)];

RunSnapshot read_rtc_run_snapshot() {
    RunSnapshot snapshot;
    std::memcpy(static_cast<void*>(&snapshot), rtc_run_snapshot, sizeof(snapshot));
    return snapshot;
}
}

HealthCounters health_counters;

void HealthCounters::init() {
    Preferences preferences;
    if (preferences.begin(kNamespace, true)) {
        stored_brownout_restarts_ = preferences.getUInt(kBrownoutsKey, 0);
        stored_panel_faults_ = preferences.getUInt(kPanelFaultsKey, 0);
        stored_panel_recoveries_ = preferences.getUInt(kPanelRecoveriesKey, 0);
        RunSnapshot stored;
        if (preferences.getBytes(kLastBrownoutKey, &stored, sizeof(stored)) == sizeof(stored) &&
            stored.valid()) {
            last_brownout_ = stored;
        }
        preferences.end();
    }
    panel_faults_at_start_ = stored_panel_faults_;
    panel_recoveries_at_start_ = stored_panel_recoveries_;
    panel_faults_.store(stored_panel_faults_);
    panel_recoveries_.store(stored_panel_recoveries_);

    // The new count is written by save_if_changed().
    const bool after_brownout = esp_reset_reason() == ESP_RST_BROWNOUT;
    brownout_restarts_.store(stored_brownout_restarts_ + (after_brownout ? 1U : 0U));
    loaded_ = true;

    if (after_brownout) {
        LOG_BLE("[STARTUP] Brownout restarts so far: %lu\n",
                static_cast<unsigned long>(brownout_restarts_.load()));
        // What the previous run noted last is what was running when the
        // supply gave way.
        const RunSnapshot before = read_rtc_run_snapshot();
        if (before.valid()) {
            last_brownout_ = before;
            last_brownout_unsaved_ = true;
            LOG_BLE("[STARTUP] At the brownout: up %lus, grind phase %u, motor %s, Wi-Fi state %u, "
                    "display %s at %u, processor %u MHz\n",
                    static_cast<unsigned long>(before.uptime_ms / 1000U),
                    static_cast<unsigned>(before.grind_phase), before.motor_on ? "on" : "off",
                    static_cast<unsigned>(before.wifi_state), before.display_on ? "on" : "off",
                    static_cast<unsigned>(before.brightness), static_cast<unsigned>(before.cpu_mhz));
        }
    }
}

void HealthCounters::note_run_state(RunSnapshot now) {
    now.seal();
    std::memcpy(rtc_run_snapshot, static_cast<const void*>(&now), sizeof(now));
}

void HealthCounters::note_panel_counts(uint32_t faults_since_start,
                                       uint32_t recoveries_since_start) {
    if (!loaded_) return;
    panel_faults_.store(panel_faults_at_start_ + faults_since_start);
    panel_recoveries_.store(panel_recoveries_at_start_ + recoveries_since_start);
}

bool HealthCounters::has_unsaved_counts() const {
    return loaded_ && (last_brownout_unsaved_ ||
                       brownout_restarts_.load() != stored_brownout_restarts_ ||
                       panel_faults_.load() != stored_panel_faults_ ||
                       panel_recoveries_.load() != stored_panel_recoveries_);
}

void HealthCounters::save_if_changed() {
    if (!has_unsaved_counts()) return;
    const uint32_t brownouts = brownout_restarts_.load();
    const uint32_t faults = panel_faults_.load();
    const uint32_t recoveries = panel_recoveries_.load();
    // Nothing is written straight after a start, when a supply that has just
    // dipped may dip again. A write that fails is tried again later, not on
    // every pass of the loop.
    const uint32_t now_ms = millis();
    if (now_ms < kFirstSaveAfterStartMs ||
        (save_tried_ && now_ms - last_save_try_ms_ < kSaveRetryMs)) {
        return;
    }
    save_tried_ = true;
    last_save_try_ms_ = now_ms;

    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) return;
    if (brownouts != stored_brownout_restarts_ &&
        preferences.putUInt(kBrownoutsKey, brownouts) == sizeof(uint32_t)) {
        stored_brownout_restarts_ = brownouts;
    }
    if (faults != stored_panel_faults_ &&
        preferences.putUInt(kPanelFaultsKey, faults) == sizeof(uint32_t)) {
        stored_panel_faults_ = faults;
    }
    if (recoveries != stored_panel_recoveries_ &&
        preferences.putUInt(kPanelRecoveriesKey, recoveries) == sizeof(uint32_t)) {
        stored_panel_recoveries_ = recoveries;
    }
    if (last_brownout_unsaved_ &&
        preferences.putBytes(kLastBrownoutKey, &last_brownout_, sizeof(last_brownout_)) ==
            sizeof(last_brownout_)) {
        last_brownout_unsaved_ = false;
    }
    preferences.end();
}
