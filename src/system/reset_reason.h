#pragma once

#include <esp_system.h>

// Why the ESP32 last restarted, as written to the startup log and reported by
// the status API. BROWNOUT means the supply fell below what the ESP32 needs.
inline const char* reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXT (Reset Pin)";
        case ESP_RST_SW: return "SW (esp_restart)";
        case ESP_RST_PANIC: return "PANIC (Exception)";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}
