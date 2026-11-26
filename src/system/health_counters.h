#pragma once

#include <atomic>
#include <cstdint>

#include "run_snapshot.h"

// Counts of supply and display trouble that are kept in flash, so they can
// still be read after the grinder has been switched off and on, and a record
// of what was running at the last brownout restart. The counts in
// DisplayManager and the last restart's reason only cover the time since the
// last start.
class HealthCounters {
public:
    // Reads the stored counts and adds this start if it followed a brownout,
    // meaning the supply fell below what the ESP32 needs.
    void init();

    // Takes the display's counts since this start.
    void note_panel_counts(uint32_t faults_since_start, uint32_t recoveries_since_start);

    // True when a count differs from what flash holds.
    bool has_unsaved_counts() const;
    // Writes counts that changed to flash. Call from the main loop while no
    // grind or firmware update is running.
    void save_if_changed();

    uint32_t brownout_restarts() const { return brownout_restarts_.load(); }
    // Times the panel's frame signal stopped while the display should have
    // been on, and times it came back.
    uint32_t panel_faults() const { return panel_faults_.load(); }
    uint32_t panel_recoveries() const { return panel_recoveries_.load(); }

    // Notes what is running now, in memory that a restart does not clear.
    // Call from the main loop on every pass; `now` need not be sealed.
    void note_run_state(RunSnapshot now);
    // What was running just before the last brownout restart, as far back as
    // flash remembers. Not valid() when none has been recorded.
    RunSnapshot last_brownout() const { return last_brownout_; }

private:
    std::atomic<uint32_t> brownout_restarts_{0};
    std::atomic<uint32_t> panel_faults_{0};
    std::atomic<uint32_t> panel_recoveries_{0};

    // Totals found in flash at this start, and the totals last written.
    uint32_t panel_faults_at_start_ = 0;
    uint32_t panel_recoveries_at_start_ = 0;
    uint32_t stored_brownout_restarts_ = 0;
    uint32_t stored_panel_faults_ = 0;
    uint32_t stored_panel_recoveries_ = 0;
    // Set once in init(); read by other tasks afterwards.
    RunSnapshot last_brownout_{};
    bool last_brownout_unsaved_ = false;
    bool loaded_ = false;
    bool save_tried_ = false;
    uint32_t last_save_try_ms_ = 0;
};

extern HealthCounters health_counters;
