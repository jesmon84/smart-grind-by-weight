#pragma once
#include "../config/hardware.h"
#if HW_DISPLAY_VARIANT_V2
#include <driver/pulse_cnt.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#else
#include <Arduino_GFX_Library.h>
#endif
#include <atomic>
#include <lvgl.h>
#include "panel_refresh_policy.h"
#include "touch_driver.h"
#include "../config/constants.h"

struct DisplayPerformanceSnapshot {
    uint32_t window_ms = 0;
    uint32_t ui_calls = 0;
    uint32_t refreshes = 0;
    uint32_t rendered_frames = 0;
    uint32_t flushes = 0;
    uint32_t pixels = 0;
    uint32_t ui_time_us = 0;
    uint32_t render_time_us = 0;
    // UI-task time spent on the panel: blocking transfers on V1, waiting for
    // a background transfer to free a draw buffer on V2.
    uint32_t flush_time_us = 0;
};

// A function the display calls when it is switched on (see
// DisplayManager::set_panel_wake_hook).
using PanelWakeHook = void (*)();

class DisplayManager {
private:
#if HW_DISPLAY_VARIANT_V2
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel_handle;
    lv_display_t* pending_flush_display;
#else
    Arduino_DataBus* bus;
    Arduino_GFX* gfx_device;
#endif
    lv_display_t* lvgl_display;
    lv_indev_t* lvgl_input;
    lv_color_t* draw_buffer;
    lv_color_t* second_draw_buffer;  // V2 only; lets LVGL render while the panel transfer runs
#if !HW_DISPLAY_VARIANT_V2
    uint16_t* dma_staging_buffer;
#endif
    TouchDriver touch_driver;
#if !HW_DISPLAY_VARIANT_V2
    uint16_t dma_staging_rows;
#endif
    
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t buffer_size;
    bool initialized;
    bool panel_powered_on;
    bool consume_wake_touch_until_release;
    uint32_t wake_touch_guard_started_ms;
    PanelWakeHook panel_wake_hook = nullptr;

    // Brightness the picture is shown at (0-255); sent with every setup of
    // the panel and every resend.
    std::atomic<uint8_t> brightness_level{0xFF};
    std::atomic<uint32_t> panel_refresh_count{0};
    // Resets since boot: those made as the display was switched off, those
    // made while it was on, by cause, and what the frame signal has done.
    std::atomic<uint32_t> panel_off_reset_count{0};
    std::atomic<uint32_t> panel_reset_counts[static_cast<size_t>(PanelResetCause::COUNT)]{};
    std::atomic<uint32_t> panel_fault_count{0};
    std::atomic<uint32_t> panel_recovery_count{0};
    std::atomic<PanelFrameState> panel_frame_state{PanelFrameState::UNKNOWN};
    // Set by request_panel_reset_from_any_task(), taken up by the UI task.
    std::atomic<bool> panel_reset_asked_elsewhere{false};
#if HW_DISPLAY_VARIANT_V2
    PanelRefreshSchedule panel_refresh;
    PanelResetSchedule panel_reset;
    PanelFrameWatch panel_frame_watch;
    PanelReveal panel_reveal;
    // Hardware counter of the pulses on the panel's frame signal; null when
    // it could not be set up, and the signal then reads as absent.
    pcnt_unit_handle_t panel_frame_counter = nullptr;
    int32_t panel_repaint_next_row = -1;  // Negative while no repaint is running
#endif

    portMUX_TYPE metrics_mux = portMUX_INITIALIZER_UNLOCKED;
    DisplayPerformanceSnapshot metrics_window;
    DisplayPerformanceSnapshot metrics_snapshot;
    uint32_t metrics_window_started_ms = 0;
    uint32_t render_started_us = 0;
#if HW_DISPLAY_VARIANT_V2
    uint32_t flush_wait_started_us = 0;
#endif

public:
    void init();
    void update();
    void set_brightness(float brightness);
    // Switches the display off or on. On V2 boards off also puts the panel to
    // sleep and resets it, and on sets it up from scratch, so a panel that
    // went dark by itself is working again each time the display wakes.
    // Either way blocks the caller for up to a quarter of a second. A finger
    // that is on the screen as it switches on is ignored until it is lifted.
    // Call from the UI task only.
    void set_panel_power(bool powered_on);
    bool is_panel_powered_on() const { return panel_powered_on; }
    // `hook` is called first thing whenever the display is switched on,
    // before the panel is set up and the screen drawn again.
    void set_panel_wake_hook(PanelWakeHook hook) { panel_wake_hook = hook; }
    // Resends the panel setup shortly in case the panel lost it, without
    // waiting for the next scheduled resend. Call from the UI task only.
    void request_panel_refresh();
    // Asks for a full reset of the panel while the display is on: its reset
    // pin is pulsed and it is set up again as at power-on, which blanks the
    // picture for about a quarter of a second. It runs once the screen has
    // been left alone for a moment, because the same pin resets the touch
    // controller. For a screen that is black; V2 boards only, and nothing
    // happens on V1. May be called from any task.
    void request_panel_reset_from_any_task() { panel_reset_asked_elsewhere.store(true); }
    static constexpr bool panel_reset_supported() { return HW_DISPLAY_VARIANT_V2 != 0; }
    // Resends of the panel setup completed since boot.
    uint32_t get_panel_refresh_count() const { return panel_refresh_count.load(); }
    // Resets of the panel since boot: made as the display was switched off,
    // and made while it was on.
    uint32_t get_panel_off_reset_count() const { return panel_off_reset_count.load(); }
    uint32_t get_panel_reset_count(PanelResetCause cause) const {
        return panel_reset_counts[static_cast<size_t>(cause)].load();
    }
    // Times since boot that the frame signal stopped while the panel should
    // have been on, and times it came back.
    uint32_t get_panel_fault_count() const { return panel_fault_count.load(); }
    uint32_t get_panel_recovery_count() const { return panel_recovery_count.load(); }
    PanelFrameState get_panel_frame_state() const { return panel_frame_state.load(); }
    uint8_t get_brightness_level() const { return brightness_level.load(); }
    bool draw_rgb565_file(const char* path, uint16_t width, uint16_t height);
    
    uint32_t get_width() const { return screen_width; }
    uint32_t get_height() const { return screen_height; }
    bool is_initialized() const { return initialized; }
    TouchDriver* get_touch_driver() { return &touch_driver; }
    DisplayPerformanceSnapshot get_performance_snapshot();
    
private:
#if HW_DISPLAY_VARIANT_V2
    void allocate_draw_buffers();
#endif
    void log_performance_window(const DisplayPerformanceSnapshot& window) const;
    static void display_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
#if HW_DISPLAY_VARIANT_V2
    static bool color_transfer_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t* event_data,
                                       void* user_context);
#endif
    static void display_rounder_cb(lv_event_t* e);
    static void display_metrics_cb(lv_event_t* e);
    static void touchpad_read_cb(lv_indev_t* indev, lv_indev_data_t* data);
    bool filter_touch_for_panel_wake(const TouchData& touch);
    void service_panel_refresh(uint32_t now_ms);
#if HW_DISPLAY_VARIANT_V2
    void send_panel_command(uint8_t command);
    void send_panel_register(uint8_t command, uint8_t value);
    void send_panel_setup();
    void continue_panel_repaint();
    bool start_panel_frame_counter();
    void watch_panel_frame_signal(uint32_t now_ms);
    void put_panel_to_sleep(bool touched);
    void wake_panel(bool touched);
    bool set_up_panel();
    void reset_panel(PanelResetCause cause);
    void restart_touch_after_panel_reset(bool was_touched);
#endif
    static uint32_t millis_cb();
};

extern DisplayManager* g_display_manager;
