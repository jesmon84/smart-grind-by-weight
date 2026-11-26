#include "touch_driver.h"
#include <Arduino.h>
#include "esp_err.h"
#include "esp_log.h"

namespace {
constexpr uint32_t kTouchI2CFrequencyHz = 300000;
constexpr int kTouchI2CTimeoutMs = 5;
constexpr char kTag[] = "TouchDriver";
// The controller reports only a few contacts. A larger count is not an answer
// from it: a bus nobody drives reads back as 0xFF, which would otherwise look
// like a touch held in the far corner.
constexpr uint8_t kMaxTouchPoints = 5;
// A stuck bus fails every poll. It is reset after this many failures in a row
// (about 0.8 s at the UI rate), at most once per interval.
constexpr uint32_t kTouchFaultsBeforeBusReset = 50;
constexpr uint32_t kTouchBusResetIntervalMs = 2000;
// The controller reports touches again about 300 ms after its reset line is
// released. Pulsing the line waits 150 ms after the release before
// controller_was_reset() is called; this covers the rest.
constexpr uint32_t kTouchRestartMs = 150;

// With ACK checking off (DEBUG_SUPPRESS_TOUCH_I2C_ERRORS) the driver no longer
// reports the controller's idle NACKs, so any error left is a failed transfer.
// With it on, a NACK arrives as ESP_ERR_INVALID_STATE and only a timeout says
// that the bus itself is stuck.
bool is_bus_fault(esp_err_t err) {
#if DEBUG_SUPPRESS_TOUCH_I2C_ERRORS
    (void)err;
    return true;
#else
    return err == ESP_ERR_TIMEOUT;
#endif
}

void suppress_touch_i2c_logs() {
#if DEBUG_SUPPRESS_TOUCH_I2C_ERRORS
    static bool configured = false;
    if (configured) {
        return;
    }
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_log_level_set("esp32-hal-i2c-ng", ESP_LOG_NONE);
    esp_log_level_set("Wire", ESP_LOG_NONE);
    configured = true;
#endif
}
}

void TouchDriver::init() {
    if (initialized) {
        return;
    }
    suppress_touch_i2c_logs();

    if (bus_handle == nullptr) {
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = static_cast<gpio_num_t>(HW_TOUCH_I2C_SDA_PIN);
        bus_config.scl_io_num = static_cast<gpio_num_t>(HW_TOUCH_I2C_SCL_PIN);
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.intr_priority = 0;
        bus_config.trans_queue_depth = 0;
        bus_config.flags.enable_internal_pullup = 1;
        bus_config.flags.allow_pd = 0;

        esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to initialize I2C bus: %s", esp_err_to_name(err));
            disabled = true;
            return;
        }
    }

    if (device_handle == nullptr) {
        i2c_device_config_t device_config = {};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = HW_TOUCH_I2C_ADDRESS;
        device_config.scl_speed_hz = kTouchI2CFrequencyHz;
        device_config.scl_wait_us = 0;
#if DEBUG_SUPPRESS_TOUCH_I2C_ERRORS
        device_config.flags.disable_ack_check = 1;  // Treat idle NACKs as benign when suppression enabled.
#else
        device_config.flags.disable_ack_check = 0;
#endif

        esp_err_t err = i2c_master_bus_add_device(bus_handle, &device_config, &device_handle);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to attach touch device: %s", esp_err_to_name(err));
            disabled = true;
            return;
        }
    }
    
    last_touch = {0, 0, false, false};
    press_event_pending = false;
    initialized = true;
    disabled = false;
    
    // Initialize touch tracking
    last_touch_time = millis();
}

void TouchDriver::update() {
    if (!initialized || disabled || device_handle == nullptr) {
        return;
    }
    if (restarting) {
        if (millis() - restart_started_ms < kTouchRestartMs) {
            return;
        }
        restarting = false;
    }

    const bool was_pressed = last_touch.pressed;
    last_touch.just_pressed = press_event_pending;

    uint8_t buf[5] = {0};
    uint8_t reg = 0x02; // FT3168_REG_NUM_TOUCHES
    esp_err_t err = i2c_master_transmit_receive(device_handle, &reg, sizeof(reg), buf, sizeof(buf), kTouchI2CTimeoutMs);
    if (err != ESP_OK) {
        // Touch controller NACKs when no touch data - treat as no-touch without logging.
        if (err != ESP_ERR_INVALID_STATE && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(kTag, "Touch poll failed: %s", esp_err_to_name(err));
        }
        read_error_count.fetch_add(1);
        if (is_bus_fault(err)) {
            note_bus_fault();
        }
        last_touch.pressed = false;
        return;
    }
    consecutive_bus_faults = 0;

    uint8_t touches = buf[0] & 0x0F;
    if (touches > kMaxTouchPoints) {
        read_error_count.fetch_add(1);
        last_touch.pressed = false;
        return;
    }
    if (touches > 0) {
        last_touch.x = ((buf[1] & 0x0F) << 8) | buf[2];
        last_touch.y = ((buf[3] & 0x0F) << 8) | buf[4];
        last_touch.pressed = true;
        if (!was_pressed) {
            press_event_pending = true;
        }
        last_touch.just_pressed = press_event_pending;

        // A held or stale controller contact is not new user activity. Only a
        // fresh press resets the display idle timer.
        if (!was_pressed) {
            last_touch_time = millis();
        }
    } else {
        if (was_pressed) {
            last_touch_time = millis();
        }
        press_event_pending = false;
        last_touch.pressed = false;
        last_touch.just_pressed = false;
    }
}

// Counts a failed poll and, once the bus has clearly stopped working, clocks
// it free and restarts the I2C controller. Touch then works again without a
// power cycle.
void TouchDriver::note_bus_fault() {
    if (++consecutive_bus_faults < kTouchFaultsBeforeBusReset) {
        return;
    }
    const uint32_t now_ms = millis();
    if (now_ms - last_bus_reset_ms < kTouchBusResetIntervalMs) {
        return;
    }
    last_bus_reset_ms = now_ms;
    consecutive_bus_faults = 0;

    const esp_err_t reset_result = i2c_master_bus_reset(bus_handle);
    const uint32_t resets = bus_reset_count.fetch_add(1) + 1;
    LOG_BLE("[TOUCH] Bus stopped answering; reset %lu: %s\n",
            static_cast<unsigned long>(resets), esp_err_to_name(reset_result));
}

void TouchDriver::disable() {
    disabled = true;
    last_touch.pressed = false;  // Clear any active touch state
    last_touch.just_pressed = false;
    press_event_pending = false;
}

void TouchDriver::enable() {
    disabled = false;
}

void TouchDriver::consume_press_event() {
    press_event_pending = false;
    last_touch.just_pressed = false;
}

uint32_t TouchDriver::get_ms_since_last_touch() const {
    if (!initialized || disabled) return 0;
    return millis() - last_touch_time;
}

bool TouchDriver::quiet_for(uint32_t ms) const {
    if (!initialized || disabled) return true;
    return !last_touch.pressed && millis() - last_touch_time >= ms;
}

void TouchDriver::controller_was_reset() {
    restarting = true;
    restart_started_ms = millis();
    last_touch.pressed = false;
    last_touch.just_pressed = false;
    press_event_pending = false;
    consecutive_bus_faults = 0;
}
