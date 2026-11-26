#pragma once

#include <cstdint>

// What the grinder was doing at a moment in time. The main loop keeps one in
// memory that a restart does not clear, so that after a brownout restart the
// firmware can say what was running when the supply gave way.
struct RunSnapshot {
    static constexpr uint32_t kMagic = 0x52554E31U;  // "RUN1"

    uint32_t magic = 0;
    uint32_t uptime_ms = 0;
    uint16_t cpu_mhz = 0;
    uint8_t grind_phase = 0;   // GrindPhase
    uint8_t motor_on = 0;
    uint8_t wifi_state = 0;    // NetworkState
    uint8_t display_on = 0;
    uint8_t brightness = 0;    // 0-255
    uint8_t checksum = 0;

    // Marks the snapshot as written in full. Call last, after the fields.
    void seal() {
        magic = kMagic;
        checksum = sum();
    }

    // False for memory that was never written, was cleared by a power cycle
    // or was caught half written by the restart.
    bool valid() const { return magic == kMagic && checksum == sum(); }

private:
    // A small hash over every field, so that changes in several fields do
    // not cancel out the way they can in a plain sum.
    uint8_t sum() const {
        uint32_t hash = 2166136261U;
        const auto mix = [&hash](uint32_t value, int bytes) {
            for (int index = 0; index < bytes; ++index) {
                hash ^= (value >> (8 * index)) & 0xFFU;
                hash *= 16777619U;
            }
        };
        mix(uptime_ms, 4);
        mix(cpu_mhz, 2);
        mix(grind_phase, 1);
        mix(motor_on, 1);
        mix(wifi_state, 1);
        mix(display_on, 1);
        mix(brightness, 1);
        return static_cast<uint8_t>(hash ^ (hash >> 8) ^ (hash >> 16) ^ (hash >> 24));
    }
};
