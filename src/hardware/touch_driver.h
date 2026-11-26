#pragma once
#include <atomic>
#include <driver/i2c_master.h>
#include "../config/constants.h"

struct TouchData {
    uint16_t x;
    uint16_t y;
    bool pressed;
    bool just_pressed;
};

class TouchDriver {
private:
    TouchData last_touch{0, 0, false, false};
    bool initialized = false;
    bool disabled = false;
    bool press_event_pending = false;
    
    // Touch activity tracking
    uint32_t last_touch_time = 0;

    // Bus recovery after repeated failed polls (for example SDA held low by
    // the controller after electrical noise)
    uint32_t consecutive_bus_faults = 0;
    uint32_t last_bus_reset_ms = 0;
    std::atomic<uint32_t> bus_reset_count{0};
    std::atomic<uint32_t> read_error_count{0};

public:
    void init();
    void update();
    void disable();
    void enable();
    TouchData get_touch_data() const { return last_touch; }
    bool is_pressed() const { return last_touch.pressed; }
    void consume_press_event();
    
    // Touch activity timing
    uint32_t get_ms_since_last_touch() const;
    // True when the screen has been left alone for `ms`. A switched-off touch
    // driver counts as left alone.
    bool quiet_for(uint32_t ms) const;

    // The display panel's reset line also resets the touch controller. Call
    // after pulsing it: a contact in progress is dropped and polling pauses
    // until the controller has restarted.
    void controller_was_reset();

    // Diagnostics since boot: bus resets made to recover the controller, and
    // polls that failed or returned no usable answer.
    uint32_t get_bus_reset_count() const { return bus_reset_count.load(); }
    uint32_t get_read_error_count() const { return read_error_count.load(); }

private:
    void note_bus_fault();

    // Set while the controller restarts after a reset of the display panel
    bool restarting = false;
    uint32_t restart_started_ms = 0;

    i2c_master_bus_handle_t bus_handle = nullptr;
    i2c_master_dev_handle_t device_handle = nullptr;
};
