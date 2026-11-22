#pragma once

#include <cstdint>
#include "../../system/screensaver_settings.h"

class UIManager;
class ScreensaverController;
class DisplayManager;

// Implements automatic screen dimming based on touch/weight activity, keeps
// the display dark during a firmware upload, and tells the power saver
// whether the display is in use.

class ScreenTimeoutController {
public:
    explicit ScreenTimeoutController(UIManager* manager);

    void register_events();
    void update();

    // Apply web/menu preference changes immediately and begin a fresh idle
    // interval so activity recorded before the save cannot postpone the new
    // timeout.
    bool apply_runtime_settings(bool verify_storage = false);

    void set_screensaver_controller(ScreensaverController* controller) {
        screensaver_controller_ = controller;
    }

private:
    void update_display_state();
    void save_power();
    void reload_settings_if_saved();
    void restore_normal_display(DisplayManager* display);
    bool is_protected_state() const;
    bool firmware_upload_running() const;
    void hold_display_off_for_upload(DisplayManager* display);
    void release_display_after_upload(DisplayManager* display);

    UIManager* ui_manager_;
    ScreensaverController* screensaver_controller_ = nullptr;
    ScreensaverTimingSettings timing_settings_;
    // ScreensaverSettings::timing_revision() that timing_settings_ was read at
    uint32_t loaded_timing_revision_ = 0;
    uint32_t settings_applied_at_ms_ = 0;
    uint32_t last_weight_activity_ms_ = 0;
    bool screen_dimmed_;
    bool panel_off_;
    // True while the display is kept dark for a firmware upload
    bool upload_hold_ = false;
};
