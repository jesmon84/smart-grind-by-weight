#pragma once

#include <cstdint>

// The grinder's 5 V supply has little to spare for this board with Wi-Fi on
// (docs/TROUBLESHOOTING.md, "Display Goes Black While the Grinder Keeps
// Working"). When it sags the display panel goes dark, and stays dark until
// the grinder is switched off and on. These rules keep the board's draw down
// at the times when that costs the user nothing.

//------------------------------------------------------------------------------
// Processor clock
//------------------------------------------------------------------------------

// The slow clock is the lowest the radio works with.
constexpr uint32_t kCpuMhzInUse = 240U;
constexpr uint32_t kCpuMhzResting = 80U;

// The clock to run at. Full speed while somebody may be looking at the
// display, while a grind runs, and while Bluetooth moves data, whose transfers
// are paced for full speed. The slow clock otherwise: with the display dimmed
// or off.
//
// Firmware arriving over Bluetooth is the exception: the start of an upload is
// the heaviest load the grinder's supply sees (a flash erase while the radio
// sends flat out), and the display's update screen blacked out there at full
// speed. While the data arrives the processor runs slowly, whatever the
// display does; applying the patch afterwards runs at full speed again.
constexpr uint32_t cpu_clock_mhz(bool display_in_use, bool grind_active, bool bluetooth_busy,
                                 bool receiving_firmware = false) {
    if (receiving_firmware) {
        return kCpuMhzResting;
    }
    return (display_in_use || grind_active || bluetooth_busy) ? kCpuMhzInUse : kCpuMhzResting;
}

//------------------------------------------------------------------------------
// Display during a firmware upload
//------------------------------------------------------------------------------
// The update screen stays on with its progress, at this part of the normal
// brightness, and never brighter than the normal brightness itself.
constexpr float kUploadBrightnessShare = 0.33f;

constexpr float upload_brightness(float normal_brightness, float minimal_brightness) {
    const float dimmed = normal_brightness * kUploadBrightnessShare;
    const float floor = minimal_brightness < normal_brightness ? minimal_brightness : normal_brightness;
    return dimmed > floor ? dimmed : floor;
}

//------------------------------------------------------------------------------
// Online update check
//------------------------------------------------------------------------------
// A check opens a secure connection, the heaviest load the processor and the
// radio make together outside a firmware upload. Soon after a start it would
// fall on a supply already strained by the upload and the restart before it.
// Releases are rare, so the first check waits half an hour, later ones come
// once a day, and a failed one is not repeated for hours. A check only starts
// while the display is dimmed or off.

constexpr uint32_t kFirstUpdateCheckDelayMs = 30U * 60U * 1000U;
constexpr uint32_t kUpdateCheckIntervalMs = 24U * 60U * 60U * 1000U;
constexpr uint32_t kUpdateCheckRetryMs = 6U * 60U * 60U * 1000U;

// `last_check_ms` is 0 until a check has run since start.
constexpr bool update_check_due(uint32_t now_ms, uint32_t last_check_ms, bool last_check_failed,
                                bool display_resting) {
    if (!display_resting) {
        return false;
    }
    if (last_check_ms == 0) {
        return now_ms >= kFirstUpdateCheckDelayMs;
    }
    return now_ms - last_check_ms >=
           (last_check_failed ? kUpdateCheckRetryMs : kUpdateCheckIntervalMs);
}
