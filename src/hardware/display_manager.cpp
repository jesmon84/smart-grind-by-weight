#include "display_manager.h"
#include "touch_wake_policy.h"
#if HW_DISPLAY_VARIANT_V2
#include "esp_lcd_sh8601.h"
#endif
#include "../config/constants.h"
#include "../config/logging.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <algorithm>
#include <esp_heap_caps.h>
#if HW_DISPLAY_VARIANT_V2
#include <driver/spi_master.h>
#else
#include <cstring>
#endif

DisplayManager* g_display_manager = nullptr;

#if HW_DISPLAY_VARIANT_V2
namespace {
constexpr spi_host_device_t kDisplaySpiHost = SPI2_HOST;

constexpr uint8_t kCmdC4[] = {0x80};
constexpr uint8_t kCmd35[] = {0x00};
constexpr uint8_t kCmd53[] = {0x20};
constexpr uint8_t kCmd63[] = {0xFF};
static const uint8_t kBrightnessOff[] = {0x00};
// Ends with the display on at zero brightness; the caller sets the brightness
// when the picture is to be seen.
static const sh8601_lcd_init_cmd_t kV2InitCommands[] = {
    {0x11, nullptr, 0, 80},
    {0xC4, kCmdC4, sizeof(kCmdC4), 0},
    {0x35, kCmd35, sizeof(kCmd35), 0},
    {0x53, kCmd53, sizeof(kCmd53), 1},
    {0x63, kCmd63, sizeof(kCmd63), 1},
    {0x51, kBrightnessOff, sizeof(kBrightnessOff), 1},
    {0x29, nullptr, 0, 10},
};

constexpr uint8_t kCmdSleepIn = 0x10;
constexpr uint8_t kCmdSleepOut = 0x11;
// Leaves "all pixels off", a black screen that Display On does not end.
constexpr uint8_t kCmdNormalDisplayOn = 0x13;
constexpr uint8_t kCmdDisplayOn = 0x29;
constexpr uint8_t kCmdBrightness = 0x51;

// The registers esp_lcd_panel_init() writes for this panel, leaving out Sleep
// Out, Display On and the brightness. Resent with those three, and with
// Normal Display On, on the schedule in panel_refresh_policy.h.
struct PanelRegister {
    uint8_t command;
    uint8_t value;
};
constexpr PanelRegister kV2SetupRegisters[] = {
    {0x36, 0x00},  // Memory access control: RGB order, no mirroring
    {0x3A, 0x55},  // Pixel format: 16 bits per pixel
    {0xC4, kCmdC4[0]},
    {0x35, kCmd35[0]},
    {0x53, kCmd53[0]},
    {0x63, kCmd63[0]},
};

// The frame signal's hardware counter starts again from zero at this count,
// and ignores pulses shorter than a real one can be.
constexpr int kPanelFrameCounterLimit = 30000;
constexpr uint32_t kPanelFrameGlitchNs = 1000;

// The redraw after a resend is spread over this many UI cycles. Drawing a
// quarter of the screen takes up to 22 ms on the V2 board, longer than one
// 16 ms UI cycle; a twelfth leaves room for the normal drawing of a frame.
constexpr int32_t kPanelRepaintBands = 12;

// QSPI command word: the write opcode, then the command as a 24-bit address.
constexpr uint32_t panel_command_word(uint8_t command) {
    return (0x02UL << 24) | (static_cast<uint32_t>(command) << 8);
}

lv_color_t* allocate_internal_dma_buffer(size_t bytes) {
    return static_cast<lv_color_t*>(heap_caps_aligned_alloc(
        LV_DRAW_BUF_ALIGN, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
}
}
#endif

void DisplayManager::init() {
    g_display_manager = this;
    panel_powered_on = true;
    consume_wake_touch_until_release = false;
    wake_touch_guard_started_ms = 0;
    
#if HW_DISPLAY_VARIANT_V2
    // V2 uses an SH8601 panel and GPIO 46 for chip select. The native
    // esp_lcd driver is required; the V1 Arduino_GFX path leaves V2 black.
    spi_bus_config_t bus_config = SH8601_PANEL_BUS_QSPI_CONFIG(
        HW_DISPLAY_SCK_PIN, HW_DISPLAY_D0_PIN, HW_DISPLAY_D1_PIN,
        HW_DISPLAY_D2_PIN, HW_DISPLAY_D3_PIN,
        HW_DISPLAY_WIDTH_PX * HW_DISPLAY_HEIGHT_PX * sizeof(uint16_t));
    if (spi_bus_initialize(kDisplaySpiHost, &bus_config, SPI_DMA_CH_AUTO) != ESP_OK) {
        return;
    }

    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(HW_DISPLAY_CS_PIN, color_transfer_done_cb, this);
    io_config.pclk_hz = HW_DISPLAY_QSPI_FREQUENCY_HZ;
    if (esp_lcd_new_panel_io_spi(
            static_cast<esp_lcd_spi_bus_handle_t>(kDisplaySpiHost),
            &io_config, &panel_io) != ESP_OK) {
        return;
    }

    sh8601_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = kV2InitCommands;
    vendor_config.init_cmds_size = sizeof(kV2InitCommands) / sizeof(kV2InitCommands[0]);
    vendor_config.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = HW_DISPLAY_RESET_PIN;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;

    if (esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel_handle) != ESP_OK ||
        esp_lcd_panel_reset(panel_handle) != ESP_OK ||
        esp_lcd_panel_init(panel_handle) != ESP_OK ||
        esp_lcd_panel_disp_on_off(panel_handle, true) != ESP_OK) {
        return;
    }
    send_panel_register(kCmdBrightness, brightness_level.load());
#else
    // Initialize V1 display hardware
    bus = new Arduino_ESP32QSPI(
        HW_DISPLAY_CS_PIN, HW_DISPLAY_SCK_PIN, HW_DISPLAY_D0_PIN, 
        HW_DISPLAY_D1_PIN, HW_DISPLAY_D2_PIN, HW_DISPLAY_D3_PIN);
    
    gfx_device = new Arduino_CO5300(
        bus, HW_DISPLAY_RESET_PIN, HW_DISPLAY_ROTATION_DEG, HW_DISPLAY_WIDTH_PX, HW_DISPLAY_HEIGHT_PX,
        HW_DISPLAY_COLOR_ORDER, HW_DISPLAY_OFFSET_X_PX, HW_DISPLAY_IPS_INVERT_X, HW_DISPLAY_IPS_INVERT_Y);

    
    if (!gfx_device->begin()) {
        return;
    }
    
    gfx_device->fillScreen(RGB565_BLACK);
#endif
    
    // Initialize LVGL
    lv_init();
    lv_tick_set_cb(millis_cb);
    
#if HW_DISPLAY_VARIANT_V2
    screen_width = HW_DISPLAY_WIDTH_PX;
    screen_height = HW_DISPLAY_HEIGHT_PX;
#else
    screen_width = gfx_device->width();
    screen_height = gfx_device->height();
#endif

    // Partial rendering: LVGL draws each refresh as horizontal strips that fit
    // the draw buffer. RGB565 format (16bit per pixel)
    draw_buffer = nullptr;
    second_draw_buffer = nullptr;
#if HW_DISPLAY_VARIANT_V2
    allocate_draw_buffers();
#else
    dma_staging_buffer = nullptr;
    dma_staging_rows = 16;

    const size_t draw_rows = 40; // 280 * 40 * 2 = 22,400 bytes
    buffer_size = screen_width * draw_rows * sizeof(uint16_t);

    draw_buffer = static_cast<lv_color_t*>(
        heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN,
                                buffer_size,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (!draw_buffer) {
        draw_buffer = static_cast<lv_color_t*>(
            heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN,
                                    buffer_size,
                                    MALLOC_CAP_8BIT));
    }
#endif

    if (!draw_buffer) {
        LOG_BLE("[DISPLAY] ERROR: Failed to allocate LVGL draw buffer\n");
        return;
    }

#if !HW_DISPLAY_VARIANT_V2
    while (dma_staging_rows >= 4 && dma_staging_buffer == nullptr) {
        dma_staging_buffer = static_cast<uint16_t*>(
            heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN,
                                    screen_width * dma_staging_rows * sizeof(uint16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        if (!dma_staging_buffer) {
            dma_staging_rows /= 2;
        }
    }

    if (!dma_staging_buffer) {
        LOG_BLE("[DISPLAY] ERROR: Failed to allocate DMA staging buffer\n");
        heap_caps_free(draw_buffer);
        draw_buffer = nullptr;
        return;
    }
#endif

    lvgl_display = lv_display_create(screen_width, screen_height);
    lv_display_set_flush_cb(lvgl_display, display_flush_cb);
    lv_display_set_buffers(lvgl_display, draw_buffer, second_draw_buffer,
                          buffer_size , LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_display_add_event_cb(lvgl_display, display_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(lvgl_display, display_metrics_cb, LV_EVENT_ALL, NULL);

    // Initialize touch
    touch_driver.init();
    lvgl_input = lv_indev_create();
    lv_indev_set_type(lvgl_input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvgl_input, touchpad_read_cb);

    // The UI task already runs LVGL every SYS_TASK_UI_INTERVAL_MS. LVGL's own
    // refresh, input and animation timers default to that same period, so a
    // little scheduling jitter made them skip a UI cycle and a frame waited
    // twice as long. With shorter periods every UI cycle can draw a frame.
    lv_timer_set_period(lv_display_get_refr_timer(lvgl_display), SYS_LVGL_TIMER_PERIOD_MS);
    lv_timer_set_period(lv_indev_get_read_timer(lvgl_input), SYS_LVGL_TIMER_PERIOD_MS);
    lv_timer_set_period(lv_anim_get_timer(), SYS_LVGL_TIMER_PERIOD_MS);

    initialized = true;

#if HW_DISPLAY_VARIANT_V2
    if (!start_panel_frame_counter()) {
        LOG_BLE("[DISPLAY] Frame signal counter could not be started; the signal is not watched\n");
    }
    const uint32_t set_up_ms = millis();
    panel_refresh.start(set_up_ms);
    panel_frame_watch.panel_set_up(set_up_ms);
    LOG_BLE("[DISPLAY] Panel setup is resent every %lus while the display is on; "
            "the panel is reset while the display is off and when its frame signal stops\n",
            static_cast<unsigned long>(kPanelRefreshIntervalMs / 1000U));
#endif
}

#if HW_DISPLAY_VARIANT_V2
// Counts the panel's frame pulses in hardware, so that counting costs no
// processor time and a noisy line cannot keep the processor busy. The UI task
// works out from the count whether the panel is running.
bool DisplayManager::start_panel_frame_counter() {
    pcnt_unit_config_t unit_config = {};
    unit_config.low_limit = -1;  // Unused; the driver wants it below zero
    unit_config.high_limit = kPanelFrameCounterLimit;
    if (pcnt_new_unit(&unit_config, &panel_frame_counter) != ESP_OK) {
        panel_frame_counter = nullptr;
        return false;
    }

    pcnt_glitch_filter_config_t filter_config = {};
    filter_config.max_glitch_ns = kPanelFrameGlitchNs;
    pcnt_chan_config_t channel_config = {};
    channel_config.edge_gpio_num = HW_DISPLAY_TE_PIN;
    channel_config.level_gpio_num = -1;
    pcnt_channel_handle_t channel = nullptr;
    const bool started =
        pcnt_unit_set_glitch_filter(panel_frame_counter, &filter_config) == ESP_OK &&
        pcnt_new_channel(panel_frame_counter, &channel_config, &channel) == ESP_OK &&
        pcnt_channel_set_edge_action(channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD) == ESP_OK &&
        pcnt_channel_set_level_action(channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                      PCNT_CHANNEL_LEVEL_ACTION_KEEP) == ESP_OK &&
        pcnt_unit_enable(panel_frame_counter) == ESP_OK &&
        pcnt_unit_clear_count(panel_frame_counter) == ESP_OK &&
        pcnt_unit_start(panel_frame_counter) == ESP_OK;
    if (!started) {
        // Left allocated: this happens once, at power-on, and a counter that
        // is not read does nothing.
        panel_frame_counter = nullptr;
    }
    return started;
}
#endif

#if HW_DISPLAY_VARIANT_V2
// Two buffers let LVGL render the next strip while the previous one is still
// being sent to the panel; with one buffer every transfer stalls rendering.
// Internal RAM is scarce once Wi-Fi and Bluetooth run, so shorter strips are
// tried before giving up the second buffer.
void DisplayManager::allocate_draw_buffers() {
    for (size_t rows = HW_DISPLAY_DRAW_BUFFER_ROWS;
         rows >= HW_DISPLAY_DRAW_BUFFER_MIN_ROWS;
         rows /= 2) {
        const size_t bytes = screen_width * rows * sizeof(uint16_t);
        draw_buffer = allocate_internal_dma_buffer(bytes);
        second_draw_buffer = draw_buffer ? allocate_internal_dma_buffer(bytes) : nullptr;
        if (second_draw_buffer) {
            buffer_size = bytes;
            LOG_BLE("[DISPLAY] Draw buffers: 2 x %u rows (%u bytes internal RAM)\n",
                    static_cast<unsigned>(rows), static_cast<unsigned>(2 * bytes));
            return;
        }
        heap_caps_free(draw_buffer);
        draw_buffer = nullptr;
    }

    buffer_size = screen_width * HW_DISPLAY_DRAW_BUFFER_MIN_ROWS * sizeof(uint16_t);
    draw_buffer = allocate_internal_dma_buffer(buffer_size);
    if (draw_buffer) {
        LOG_BLE("[DISPLAY] Draw buffer: 1 x %u rows; not enough internal RAM for two\n",
                static_cast<unsigned>(HW_DISPLAY_DRAW_BUFFER_MIN_ROWS));
    }
}
#endif

void DisplayManager::update() {
    if (!initialized) return;

    const uint32_t started_us = micros();
    touch_driver.update();
    lv_timer_handler();

    const uint32_t now_ms = millis();
    metrics_window.ui_calls++;
    metrics_window.ui_time_us += micros() - started_us;
    if (metrics_window_started_ms == 0) {
        metrics_window_started_ms = now_ms;
    } else if (now_ms - metrics_window_started_ms >= 1000) {
        metrics_window.window_ms = now_ms - metrics_window_started_ms;
        portENTER_CRITICAL(&metrics_mux);
        metrics_snapshot = metrics_window;
        portEXIT_CRITICAL(&metrics_mux);
        log_performance_window(metrics_window);
        metrics_window = {};
        metrics_window_started_ms = now_ms;
    }

    // After drawing, so its short pause falls in the idle part of the cycle.
    service_panel_refresh(now_ms);
}

void DisplayManager::log_performance_window(const DisplayPerformanceSnapshot& window) const {
#if DEBUG_DISPLAY_PERFORMANCE
    // Skip idle seconds so the log only covers periods when the screen redraws.
    if (window.rendered_frames == 0 || window.window_ms == 0) return;

    const unsigned long frames = window.rendered_frames;
    const unsigned long fps_x10 = (frames * 10000UL) / window.window_ms;
    LOG_DISPLAY_PERF(
        "[DISPLAY] %lu.%lu fps | frame %lu us | panel %lu us/frame | %lu px/frame | "
        "internal RAM %u KiB free (largest block %u KiB)\n",
        fps_x10 / 10, fps_x10 % 10,
        static_cast<unsigned long>(window.render_time_us) / frames,
        static_cast<unsigned long>(window.flush_time_us) / frames,
        static_cast<unsigned long>(window.pixels) / frames,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
#else
    (void)window;
#endif
}

DisplayPerformanceSnapshot DisplayManager::get_performance_snapshot() {
    portENTER_CRITICAL(&metrics_mux);
    const DisplayPerformanceSnapshot snapshot = metrics_snapshot;
    portEXIT_CRITICAL(&metrics_mux);
    return snapshot;
}

bool DisplayManager::draw_rgb565_file(const char* path, uint16_t width, uint16_t height) {
#if HW_DISPLAY_VARIANT_V2
    // The SH8601 path submits transfers asynchronously.  The normal LVGL
    // startup screensaver remains available on V2, but this early, direct
    // Arduino_GFX optimisation is V1-only until it has a dedicated transfer
    // completion path.  Do not reference the V1-only gfx_device here: doing so
    // makes the V2 firmware fail to compile.
    (void)path;
    (void)width;
    (void)height;
    return false;
#else
    if (!initialized || !gfx_device || !path || width == 0 || height == 0) {
        return false;
    }

    const size_t expected_size = static_cast<size_t>(width) * height * sizeof(uint16_t);
    File file = LittleFS.open(path, "r");
    if (!file) {
        LOG_BLE("[DISPLAY] RGB565 draw skipped: failed to open %s\n", path);
        return false;
    }

    if (file.size() != expected_size) {
        LOG_BLE("[DISPLAY] RGB565 draw skipped: %s size %u != %u\n",
                path,
                static_cast<unsigned>(file.size()),
                static_cast<unsigned>(expected_size));
        file.close();
        return false;
    }

    uint16_t rows_per_chunk = 16;
    uint16_t* row_buffer = nullptr;
    while (rows_per_chunk >= 4 && row_buffer == nullptr) {
        row_buffer = static_cast<uint16_t*>(
            heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN,
                                    static_cast<size_t>(width) * rows_per_chunk * sizeof(uint16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        if (!row_buffer) {
            rows_per_chunk /= 2;
        }
    }

    if (!row_buffer) {
        LOG_BLE("[DISPLAY] RGB565 draw skipped: row buffer allocation failed\n");
        file.close();
        return false;
    }

    const int16_t x = static_cast<int16_t>((screen_width > width) ? ((screen_width - width) / 2) : 0);
    const int16_t y = static_cast<int16_t>((screen_height > height) ? ((screen_height - height) / 2) : 0);
    bool success = true;

    for (uint16_t current_y = 0; current_y < height; current_y += rows_per_chunk) {
        const uint16_t rows = std::min<uint16_t>(rows_per_chunk, height - current_y);
        const size_t bytes_to_read = static_cast<size_t>(width) * rows * sizeof(uint16_t);
        const size_t bytes_read = file.read(reinterpret_cast<uint8_t*>(row_buffer), bytes_to_read);
        if (bytes_read != bytes_to_read) {
            LOG_BLE("[DISPLAY] RGB565 draw aborted: short read at row %u\n", current_y);
            success = false;
            break;
        }

        // Screensaver uploads are stored as native little-endian RGB565.
        // Arduino_GFX converts native pixels to the panel byte order here.
        gfx_device->draw16bitRGBBitmap(x, y + current_y, row_buffer, width, rows);
    }

    heap_caps_free(row_buffer);
    file.close();
    return success;
#endif
}

// Update the refresh area to be full width
// This avoids weird artifacts when partial row updates are used
// Rows start on an even line and end on an odd one: the SH8601 (V2) only
// accepts drawing windows on 2-pixel boundaries.
void DisplayManager::display_rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);

    area->x1 = 0;
    area->x2 = g_display_manager->screen_width - 1;
    area->y1 &= ~1;
    area->y2 = std::min<int32_t>(area->y2 | 1, g_display_manager->screen_height - 1);
}

void DisplayManager::display_metrics_cb(lv_event_t* e) {
    if (!g_display_manager) return;

    switch (lv_event_get_code(e)) {
        case LV_EVENT_REFR_START:
            g_display_manager->metrics_window.refreshes++;
            break;
        case LV_EVENT_RENDER_START:
            g_display_manager->metrics_window.rendered_frames++;
            g_display_manager->render_started_us = micros();
            break;
        case LV_EVENT_RENDER_READY:
            if (g_display_manager->render_started_us != 0) {
                g_display_manager->metrics_window.render_time_us +=
                    micros() - g_display_manager->render_started_us;
                g_display_manager->render_started_us = 0;
            }
            break;
#if HW_DISPLAY_VARIANT_V2
        // V2 transfers run in the background, so the panel only costs the UI
        // the time LVGL waits for a transfer to free a draw buffer.
        case LV_EVENT_FLUSH_WAIT_START:
            g_display_manager->flush_wait_started_us = micros();
            break;
        case LV_EVENT_FLUSH_WAIT_FINISH:
            g_display_manager->metrics_window.flush_time_us +=
                micros() - g_display_manager->flush_wait_started_us;
            break;
        // A redraw has been handed to the panel. A panel that has just been
        // set up can now show it (see PanelReveal).
        case LV_EVENT_REFR_READY:
            g_display_manager->panel_reveal.screen_drawn();
            break;
#endif
        default:
            break;
    }
}

void DisplayManager::display_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
#if HW_DISPLAY_VARIANT_V2
    // A panel that is switched off is asleep and takes no picture; the whole
    // screen is drawn again when it wakes.
    if (!g_display_manager || !g_display_manager->panel_handle ||
        !g_display_manager->panel_powered_on) {
        lv_display_flush_ready(disp);
        return;
    }

    g_display_manager->metrics_window.flushes++;
    g_display_manager->metrics_window.pixels += lv_area_get_size(area);

    g_display_manager->pending_flush_display = disp;
    esp_err_t result = esp_lcd_panel_draw_bitmap(
        g_display_manager->panel_handle,
        area->x1 + HW_DISPLAY_OFFSET_X_PX, area->y1,
        area->x2 + HW_DISPLAY_OFFSET_X_PX + 1, area->y2 + 1,
        px_map);
    if (result != ESP_OK) {
        g_display_manager->pending_flush_display = nullptr;
        lv_display_flush_ready(disp);
    }
#else
    if (!g_display_manager || !g_display_manager->gfx_device) return;

    const uint32_t flush_started_us = micros();
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);

    uint32_t remaining_rows = h;
    uint32_t current_y = area->y1;
    uint32_t src_row_offset = 0;
    uint16_t* staging = g_display_manager->dma_staging_buffer;
    uint32_t staging_rows = g_display_manager->dma_staging_rows ? g_display_manager->dma_staging_rows : h;
    const uint16_t* src_pixels = reinterpret_cast<const uint16_t*>(px_map);

    while (remaining_rows > 0) {
        uint32_t rows = std::min<uint32_t>(remaining_rows, staging_rows);
        size_t copy_pixels = static_cast<size_t>(w) * rows;
        const uint16_t* chunk_src = src_pixels + (static_cast<size_t>(src_row_offset) * w);

        if (staging) {
            memcpy(staging, chunk_src, copy_pixels * sizeof(uint16_t));

            if (LV_COLOR_16_SWAP) {
                g_display_manager->gfx_device->draw16bitBeRGBBitmap(area->x1, current_y, staging, w, rows);
            } else {
                g_display_manager->gfx_device->draw16bitRGBBitmap(area->x1, current_y, staging, w, rows);
            }
        } else {
            if (LV_COLOR_16_SWAP) {
                g_display_manager->gfx_device->draw16bitBeRGBBitmap(area->x1, current_y, const_cast<uint16_t*>(chunk_src), w, rows);
            } else {
                g_display_manager->gfx_device->draw16bitRGBBitmap(area->x1, current_y, const_cast<uint16_t*>(chunk_src), w, rows);
            }
        }

        remaining_rows -= rows;
        src_row_offset += rows;
        current_y += rows;
    }

    g_display_manager->metrics_window.flushes++;
    g_display_manager->metrics_window.pixels += w * h;
    g_display_manager->metrics_window.flush_time_us += micros() - flush_started_us;

    lv_display_flush_ready(disp);
#endif
}

#if HW_DISPLAY_VARIANT_V2
bool DisplayManager::color_transfer_done_cb(esp_lcd_panel_io_handle_t,
                                            esp_lcd_panel_io_event_data_t*,
                                            void* user_context) {
    DisplayManager* manager = static_cast<DisplayManager*>(user_context);
    if (manager && manager->pending_flush_display) {
        lv_display_t* display = manager->pending_flush_display;
        manager->pending_flush_display = nullptr;
        lv_display_flush_ready(display);
    }
    return false;
}
#endif

void DisplayManager::touchpad_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (!g_display_manager) return;
    
    TouchData touch = g_display_manager->touch_driver.get_touch_data();

    if (g_display_manager->filter_touch_for_panel_wake(touch)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    
    if (touch.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touch.x;
        data->point.y = touch.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

uint32_t DisplayManager::millis_cb() {
    return millis();
}

void DisplayManager::set_brightness(float brightness) {
#if HW_DISPLAY_VARIANT_V2
    if (!initialized || !panel_io) return;
#else
    if (!initialized || !gfx_device) return;
#endif
    
    // Clamp brightness to valid hardware range [0.0, 1.0]
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    
    const uint8_t level = static_cast<uint8_t>(brightness * 255.0f);
    brightness_level.store(level);
#if HW_DISPLAY_VARIANT_V2
    // A panel that is asleep, or set up and not yet showing its picture, gets
    // the level when the picture is shown.
    if (panel_powered_on && !panel_reveal.hidden()) {
        send_panel_register(kCmdBrightness, level);
    }
#else
    // Cast to CO5300 and call setBrightness with 8-bit value
    Arduino_CO5300* display = static_cast<Arduino_CO5300*>(gfx_device);
    display->setBrightness(level);
#endif
}

void DisplayManager::set_panel_power(bool powered_on) {
#if HW_DISPLAY_VARIANT_V2
    if (!initialized || !panel_handle) return;
#else
    if (!initialized || !gfx_device) return;
#endif
    if (panel_powered_on == powered_on) return;
    if (powered_on && panel_wake_hook) {
        panel_wake_hook();
    }
    const bool touched = touch_driver.is_pressed();

#if HW_DISPLAY_VARIANT_V2
    // Off is asleep and reset; on is reset again and set up from scratch. A
    // panel that went dark by itself, while on or off, therefore works again
    // at the next wake, and the reset's blank screen falls where the screen
    // is dark anyway.
    if (powered_on) {
        wake_panel(touched);
    } else {
        put_panel_to_sleep(touched);
    }
#else
    if (powered_on) {
        gfx_device->displayOn();
    } else {
        gfx_device->displayOff();
    }
#endif

    panel_powered_on = powered_on;
    if (powered_on) {
        if (touched) {
            // A finger on the screen as it wakes went down on a dark screen,
            // whichever part of the firmware does the waking. The contact is
            // not for the control underneath it.
            touch_driver.consume_press_event();
            consume_wake_touch_until_release = true;
            wake_touch_guard_started_ms = millis();
        }
        if (lvgl_display) {
            lv_obj_invalidate(lv_screen_active());
        }
    }
    LOG_BLE("[DISPLAY] Panel %s\n", powered_on ? "on" : "off");
}

void DisplayManager::request_panel_refresh() {
#if HW_DISPLAY_VARIANT_V2
    panel_refresh.request();
#endif
}

// Keeps the panel showing a picture while it should be on: resets it when a
// reset has been asked for or its frame signal stays lost, and otherwise
// resends its setup on schedule (see panel_refresh_policy.h). A healthy panel
// shows no change from a resend. Only the V2 panel is covered; the V1 driver
// is unchanged.
void DisplayManager::service_panel_refresh(uint32_t now_ms) {
#if HW_DISPLAY_VARIANT_V2
    if (!panel_io || !panel_handle) return;

    watch_panel_frame_signal(now_ms);
    if (panel_reset_asked_elsewhere.exchange(false)) {
        panel_reset.request(PanelResetCause::REQUESTED, now_ms);
    }
    if (panel_reset.due(now_ms, panel_powered_on,
                        touch_driver.quiet_for(kPanelResetTouchQuietMs))) {
        reset_panel(panel_reset.begin());
        return;
    }
    if (panel_powered_on && panel_reveal.due(now_ms)) {
        // Sent after the last strip of the redraw has reached the panel: the
        // driver finishes the transfers under way before a command.
        send_panel_register(kCmdBrightness, brightness_level.load());
    }

    switch (panel_refresh.next_step(now_ms, panel_powered_on)) {
        case PanelRefreshStep::SLEEP_OUT:
            send_panel_command(kCmdSleepOut);
            // The controller takes no other command for 5 ms after Sleep Out.
            // Only this task talks to the panel, so waiting here guarantees it.
            vTaskDelay(pdMS_TO_TICKS(kPanelSleepOutQuietMs));
            send_panel_setup();
            break;
        case PanelRefreshStep::DISPLAY_ON:
            send_panel_command(kCmdDisplayOn);
            send_panel_register(kCmdBrightness, brightness_level.load());
            // A reset panel ignored everything drawn since, so draw it again.
            panel_repaint_next_row = 0;
            panel_refresh_count.fetch_add(1);
            break;
        case PanelRefreshStep::NONE:
            break;
    }

    continue_panel_repaint();
#else
    (void)now_ms;
#endif
}

#if HW_DISPLAY_VARIANT_V2
void DisplayManager::send_panel_command(uint8_t command) {
    esp_lcd_panel_io_tx_param(panel_io, panel_command_word(command), nullptr, 0);
}

void DisplayManager::send_panel_register(uint8_t command, uint8_t value) {
    // `value` is a copy on this task's stack: internal RAM that the SPI driver
    // sends from directly, without allocating a transfer buffer.
    esp_lcd_panel_io_tx_param(panel_io, panel_command_word(command), &value, sizeof(value));
}

void DisplayManager::send_panel_setup() {
    send_panel_command(kCmdNormalDisplayOn);
    for (const PanelRegister& panel_register : kV2SetupRegisters) {
        send_panel_register(panel_register.command, panel_register.value);
    }
    send_panel_register(kCmdBrightness, brightness_level.load());
}

// Follows the panel's frame signal. When it stops the setup is resent at
// once, and PanelResetSchedule asks for a reset if that does not bring it
// back.
void DisplayManager::watch_panel_frame_signal(uint32_t now_ms) {
    // The counter starts again from zero at its limit; the watch only needs
    // to see that the count moved.
    int pulse_count = 0;
    if (panel_frame_counter) {
        pcnt_unit_get_count(panel_frame_counter, &pulse_count);
    }
    const PanelFrameState state = panel_frame_watch.update(
        now_ms, static_cast<uint32_t>(pulse_count), panel_powered_on);
    panel_frame_state.store(state);

    const PanelFrameEvents events = panel_reset.follow_frame(now_ms, state, panel_powered_on);
    if (events.lost) {
        panel_fault_count.fetch_add(1);
        panel_refresh.request();
        LOG_BLE("[DISPLAY] Frame signal stopped: the panel has restarted or fallen asleep\n");
    }
    if (events.recovered) {
        panel_recovery_count.fetch_add(1);
        LOG_BLE("[DISPLAY] Frame signal is back\n");
    }
}

// Switches the panel off in the order its controller gives for powering down
// (Display Off, Sleep In, a pause, the reset pin) and leaves it as a reset
// does: asleep, with its supply converter stopped. wake_panel() brings it
// back. Blocks the UI task for about a quarter of a second.
void DisplayManager::put_panel_to_sleep(bool touched) {
    const bool switched_off = esp_lcd_panel_disp_on_off(panel_handle, false) == ESP_OK;
    send_panel_command(kCmdSleepIn);
    vTaskDelay(pdMS_TO_TICKS(kPanelSleepInSettleMs));
    const bool reset = esp_lcd_panel_reset(panel_handle) == ESP_OK;
    restart_touch_after_panel_reset(touched);

    panel_repaint_next_row = -1;
    panel_off_reset_count.fetch_add(1);
    if (!switched_off || !reset) {
        LOG_BLE("[DISPLAY] Panel off: a command failed\n");
    }
}

// Brings back a panel that put_panel_to_sleep() switched off. The reset pin
// is pulsed once more first, so that a panel upset while it slept starts
// clean as well. With a finger on the screen it is not pulsed: that restarts
// the touch controller, which would drop the contact and read it again as a
// new one. Blocks the UI task for about a quarter of a second, or a tenth
// with a finger on the screen.
void DisplayManager::wake_panel(bool touched) {
    const bool reset = touched || esp_lcd_panel_reset(panel_handle) == ESP_OK;
    const bool set_up = set_up_panel();
    if (!touched) {
        restart_touch_after_panel_reset(false);
    }
    if (!reset || !set_up) {
        LOG_BLE("[DISPLAY] Panel on: a setup command failed\n");
    }
}

// Sets up a panel that has been reset, with the commands used at power-on,
// and leaves its brightness at zero until the screen has been drawn again
// (see PanelReveal). The caller asks for that drawing. Blocks the UI task for
// about a tenth of a second.
bool DisplayManager::set_up_panel() {
    const bool set_up = esp_lcd_panel_init(panel_handle) == ESP_OK &&
                        esp_lcd_panel_disp_on_off(panel_handle, true) == ESP_OK;
    const uint32_t done_ms = millis();
    panel_refresh.start(done_ms);
    panel_frame_watch.panel_set_up(done_ms);
    panel_reveal.hide(done_ms);
    panel_repaint_next_row = -1;
    return set_up;
}

// Does to the panel what a restart does, while the display is on: pulses its
// reset pin and sets it up from scratch. The picture is black meanwhile, and
// the UI task blocked, for about a quarter of a second.
void DisplayManager::reset_panel(PanelResetCause cause) {
    const bool touched = touch_driver.is_pressed();
    const bool reset = esp_lcd_panel_reset(panel_handle) == ESP_OK;
    const bool set_up = set_up_panel();
    restart_touch_after_panel_reset(touched);

    panel_reset.finished(millis());
    if (lvgl_display) {
        lv_obj_invalidate(lv_screen_active());
    }
    panel_reset_counts[static_cast<size_t>(cause)].fetch_add(1);
    LOG_BLE("[DISPLAY] Panel reset (%s)%s\n", panel_reset_cause_name(cause),
            reset && set_up ? "" : ": a setup command failed");
}

// The reset pin is shared with the touch controller, which restarts too. A
// press under way ends there: what was pressed is told that the press was
// lost, which stops a held +/- button and gives no tap. A finger still down
// when the controller answers again is read as a new press.
void DisplayManager::restart_touch_after_panel_reset(bool was_touched) {
    touch_driver.controller_was_reset();
    if (was_touched && lvgl_input) {
        lv_indev_wait_release(lvgl_input);
    }
}

// Invalidates the next band of the screen; LVGL redraws it on its next cycle.
void DisplayManager::continue_panel_repaint() {
    if (panel_repaint_next_row < 0) return;
    if (!panel_powered_on || !lvgl_display) {
        panel_repaint_next_row = -1;
        return;
    }

    const int32_t height = static_cast<int32_t>(screen_height);
    // Even, because the SH8601 only accepts windows on 2-pixel boundaries.
    const int32_t band_rows = ((height / kPanelRepaintBands) + 1) & ~1;
    lv_area_t band;
    band.x1 = 0;
    band.x2 = static_cast<int32_t>(screen_width) - 1;
    band.y1 = panel_repaint_next_row;
    band.y2 = std::min(panel_repaint_next_row + band_rows, height) - 1;
    lv_obj_invalidate_area(lv_screen_active(), &band);

    panel_repaint_next_row += band_rows;
    if (panel_repaint_next_row >= height) {
        panel_repaint_next_row = -1;
    }
}
#endif

bool DisplayManager::filter_touch_for_panel_wake(const TouchData& touch) {
    if (!panel_powered_on) {
        if (touch.just_pressed) {
            // Waking also swallows this contact until it is released.
            set_panel_power(true);
            LOG_BLE("[DISPLAY] Panel wake source: touch\n");
        }
        return true;
    }

    if (!consume_wake_touch_until_release) {
        if (touch.just_pressed) {
            touch_driver.consume_press_event();
            // If the screen is black because the panel lost its setup, this
            // touch brings it back without waiting for the scheduled resend.
            request_panel_refresh();
        }
        return false;
    }
    if (!touch.pressed) {
        consume_wake_touch_until_release = false;
        wake_touch_guard_started_ms = 0;
        return true;
    }

    // Do not let a stale controller contact suppress every future gesture.
    // One second is comfortably longer than a normal wake tap, while keeping
    // recovery finite if the controller never reports the matching release.
    if (wake_touch_guard_expired(millis() - wake_touch_guard_started_ms)) {
        consume_wake_touch_until_release = false;
        wake_touch_guard_started_ms = 0;
        LOG_BLE("[DISPLAY] Wake-touch guard recovered after missing release\n");
    }
    return true;
}
