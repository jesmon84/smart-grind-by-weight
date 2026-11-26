#include "screen_timeout_controller.h"
#include "screensaver_controller.h"

#include <Arduino.h>

#include "../../config/constants.h"
#include "../../config/logging.h"
#include "../../hardware/display_manager.h"
#include "../../hardware/hardware_manager.h"
#include "../../network/device_web_server.h"
#include "../../system/display_idle_policy.h"
#include "../../system/power_saver.h"
#include "../ui_manager.h"

ScreenTimeoutController::ScreenTimeoutController(UIManager* manager)
    : ui_manager_(manager)
    , timing_settings_(ScreensaverSettings::load_timing())
    , loaded_timing_revision_(ScreensaverSettings::timing_revision())
    , settings_applied_at_ms_(millis())
    , last_weight_activity_ms_(settings_applied_at_ms_)
    , screen_dimmed_(false)
    , panel_off_(false) {}

void ScreenTimeoutController::register_events() {
    if (!ui_manager_ || !ui_manager_->hardware_manager) {
        return;
    }
    // A touch wakes the display from inside the display code, a cycle before
    // update() hears of it. The processor is at full speed for that redraw
    // all the same.
    if (DisplayManager* display = ui_manager_->hardware_manager->get_display()) {
        display->set_panel_wake_hook([] { power_saver.display_waking(); });
    }
}

void ScreenTimeoutController::update() {
    update_display_state();
    save_power();
}

// The processor runs slowly while nobody is using the display (see
// power_saving_policy.h).
void ScreenTimeoutController::save_power() {
    if (!ui_manager_) {
        return;
    }
    const bool grind_active = ui_manager_->grind_controller &&
                              ui_manager_->grind_controller->is_active();
    const bool bluetooth_busy = ui_manager_->bluetooth_manager &&
                                ui_manager_->bluetooth_manager->is_transfer_active();
    const bool receiving_firmware = ui_manager_->bluetooth_manager &&
                                    ui_manager_->bluetooth_manager->is_updating() &&
                                    ui_manager_->bluetooth_manager->get_ota_progress() < 100.0f;
    power_saver.update(!screen_dimmed_ && !panel_off_, grind_active, bluetooth_busy, receiving_firmware);
}

void ScreenTimeoutController::update_display_state() {
    if (!ui_manager_) {
        return;
    }

    auto* hardware = ui_manager_->hardware_manager;
    if (!hardware) {
        return;
    }

    auto* display = hardware->get_display();
    if (!display) {
        return;
    }

    if (firmware_upload_running()) {
        show_upload_dimmed(display);
        return;
    }
    if (upload_dimmed_) {
        end_upload_display(display);
    }

    if (is_protected_state()) {
        restore_normal_display(display);
        return;
    }

    uint32_t now = millis();
    reload_settings_if_saved();

    auto* touch_driver = display->get_touch_driver();
    if (!touch_driver) {
        return;
    }

    uint32_t ms_since_touch = touch_driver->get_ms_since_last_touch();
    auto* sensor = hardware->get_weight_sensor();
    uint32_t idle_timeout_ms = ScreensaverSettings::idle_timeout_ms(timing_settings_);
    uint32_t display_off_timeout_ms =
        ScreensaverSettings::display_off_timeout_ms(timing_settings_);
    const uint32_t ms_since_settings_applied = now - settings_applied_at_ms_;
    float weight_delta_g = 0.0f;
    if (sensor && sensor->get_weight_delta(1000U, &weight_delta_g) &&
        display_weight_activity_detected(weight_delta_g, USER_WEIGHT_ACTIVITY_THRESHOLD_G)) {
        last_weight_activity_ms_ = now;
    }

    // Use the age of the most recent real activity. Comparing the ends of a
    // short weight window rejects isolated scale noise that could otherwise
    // wake the panel repeatedly from a long min/max history.
    const uint32_t ms_since_weight = now - last_weight_activity_ms_;
    const uint32_t inactive_ms = min(min(ms_since_touch, ms_since_weight),
                                     ms_since_settings_applied);
    const DisplayIdleState idle_state = display_idle_state(
        inactive_ms,
        idle_timeout_ms,
        timing_settings_.display_off_enabled,
        static_cast<uint32_t>(timing_settings_.display_off_delay_s) * 1000U);
    const bool should_dim = idle_state != DisplayIdleState::ACTIVE;
    const bool should_turn_off = idle_state == DisplayIdleState::PANEL_OFF;

    if (should_turn_off && !panel_off_) {
        if (!screen_dimmed_) {
            float dimmed = USER_SCREEN_BRIGHTNESS_DIMMED;
            if (ui_manager_->menu_controller_) {
                dimmed = ui_manager_->menu_controller_->get_screensaver_brightness();
            }
            display->set_brightness(dimmed);
            screen_dimmed_ = true;
            if (screensaver_controller_ &&
                screensaver_controller_->is_sleep_enabled() &&
                screensaver_controller_->has_image()) {
                screensaver_controller_->show();
            }
        }
        display->set_panel_power(false);
        panel_off_ = true;
        LOG_BLE("[DISPLAY] Panel off after %lus idle (%lus after screensaver)\n",
                static_cast<unsigned long>(display_off_timeout_ms / 1000U),
                static_cast<unsigned long>(timing_settings_.display_off_delay_s));
    } else if (should_dim && !screen_dimmed_) {
        float dimmed = USER_SCREEN_BRIGHTNESS_DIMMED;
        if (ui_manager_->menu_controller_) {
            dimmed = ui_manager_->menu_controller_->get_screensaver_brightness();
        }
        display->set_brightness(dimmed);
        screen_dimmed_ = true;

        // Show screensaver image if enabled
        if (screensaver_controller_ &&
            screensaver_controller_->is_sleep_enabled() &&
            screensaver_controller_->has_image()) {
            screensaver_controller_->show();
        }
        LOG_BLE("[DISPLAY] Idle timeout reached after %lus; dimmed with screensaver=%s\n",
                static_cast<unsigned long>(timing_settings_.idle_timeout_s),
                screensaver_controller_ && screensaver_controller_->is_visible() ? "ON" : "OFF");
    } else if (!should_dim && (screen_dimmed_ || panel_off_)) {
        restore_normal_display(display);
    }
}

bool ScreenTimeoutController::apply_runtime_settings(bool verify_storage) {
    ScreensaverTimingSettings loaded;
    if (verify_storage) {
        if (!ScreensaverSettings::load_timing_checked(loaded)) return false;
    } else {
        loaded = ScreensaverSettings::load_timing();
    }
    timing_settings_ = loaded;
    settings_applied_at_ms_ = millis();
    last_weight_activity_ms_ = settings_applied_at_ms_;
    LOG_BLE("[DISPLAY] Runtime settings applied; screensaver=%lus panel-off=%s delay=%lus\n",
            static_cast<unsigned long>(timing_settings_.idle_timeout_s),
            timing_settings_.display_off_enabled ? "ON" : "OFF",
            static_cast<unsigned long>(timing_settings_.display_off_delay_s));

    if (!ui_manager_ || !ui_manager_->hardware_manager) return false;
    DisplayManager* display = ui_manager_->hardware_manager->get_display();
    if (!display) return false;
    restore_normal_display(display);
    return true;
}

// The timing settings can also be saved over Bluetooth, on another task. The
// revision says when a save happened, so flash is read only then instead of
// on a timer in the UI loop. Values that cannot be read back keep the ones in
// use.
void ScreenTimeoutController::reload_settings_if_saved() {
    const uint32_t revision = ScreensaverSettings::timing_revision();
    if (revision == loaded_timing_revision_) {
        return;
    }
    loaded_timing_revision_ = revision;

    ScreensaverTimingSettings loaded;
    if (ScreensaverSettings::load_timing_checked(loaded)) {
        timing_settings_ = loaded;
    }
}

void ScreenTimeoutController::restore_normal_display(DisplayManager* display) {
    bool screensaver_visible = screensaver_controller_ && screensaver_controller_->is_visible();
    if (screensaver_visible) {
        screensaver_controller_->hide();
    }

    if (screen_dimmed_ || panel_off_ || screensaver_visible) {
        float normal = USER_SCREEN_BRIGHTNESS_NORMAL;
        if (ui_manager_ && ui_manager_->menu_controller_) {
            normal = ui_manager_->menu_controller_->get_normal_brightness();
        }
        display->set_brightness(normal);
        // Nobody saw what happened to the panel while it was dim, and waking
        // is when a dark one would be noticed. A resend of its setup brings
        // back a panel that restarted and shows no change on a healthy one. A
        // panel that was off is set up from scratch below.
        display->request_panel_refresh();
    }

    if (panel_off_ || !display->is_panel_powered_on()) {
        display->set_panel_power(true);
    }

    screen_dimmed_ = false;
    panel_off_ = false;
}

// True while firmware is being uploaded, from the web page, over Bluetooth or
// from the grinder's own menu.
bool ScreenTimeoutController::firmware_upload_running() const {
    if (device_web_server.is_ota_in_progress()) {
        return true;
    }
    return ui_manager_ && ui_manager_->bluetooth_manager &&
           ui_manager_->bluetooth_manager->is_updating();
}

// A firmware upload is the heaviest load the grinder's supply sees. The
// update screen stays on, so the upload does not look like a broken grinder,
// but dimmed (power_saving_policy.h) until the grinder restarts into the new
// firmware.
void ScreenTimeoutController::show_upload_dimmed(DisplayManager* display) {
    if (upload_dimmed_) {
        return;
    }
    upload_dimmed_ = true;
    if (screensaver_controller_ && screensaver_controller_->is_visible()) {
        screensaver_controller_->hide();
    }
    float normal = USER_SCREEN_BRIGHTNESS_NORMAL;
    if (ui_manager_->menu_controller_) {
        normal = ui_manager_->menu_controller_->get_normal_brightness();
    }
    display->set_brightness(upload_brightness(normal, HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT / 100.0f));
    if (!display->is_panel_powered_on()) {
        display->set_panel_power(true);
    }
    // Leaves the display counted as dimmed, so that it returns to its normal
    // brightness if the upload ends without a restart.
    screen_dimmed_ = true;
    panel_off_ = false;
    LOG_BLE("[DISPLAY] Update screen dimmed for the firmware upload\n");
}

// The upload ended without a restart: it failed or was called off. The
// display comes back as after any activity, with a fresh idle interval.
void ScreenTimeoutController::end_upload_display(DisplayManager* display) {
    upload_dimmed_ = false;
    last_weight_activity_ms_ = millis();
    restore_normal_display(display);
    LOG_BLE("[DISPLAY] Normal brightness: the firmware upload ended without a restart\n");
}

bool ScreenTimeoutController::is_protected_state() const {
    if (!ui_manager_) {
        return false;
    }

    bool ota_active = ui_manager_->bluetooth_manager &&
                      ui_manager_->bluetooth_manager->is_updating();
    if (ota_active) {
        return true;
    }

    if (!ui_manager_->state_machine) {
        return false;
    }

    UIState state = ui_manager_->state_machine->get_current_state();
    if (state == UIState::GRIND_COMPLETE && ui_manager_->grinding_controller_ &&
        ui_manager_->grinding_controller_->is_result_timer_running()) {
        return true;  // Keep the counting finished screen readable
    }
    return state == UIState::GRINDING ||
           state == UIState::OTA_UPDATE ||
           state == UIState::OTA_UPDATE_FAILED;
}
