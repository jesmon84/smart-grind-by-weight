// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>

#include "system/run_snapshot.h"

namespace {

RunSnapshot sample() {
    RunSnapshot snapshot;
    snapshot.uptime_ms = 123456789U;
    snapshot.cpu_mhz = 240;
    snapshot.grind_phase = 9;
    snapshot.motor_on = 1;
    snapshot.wifi_state = 3;
    snapshot.display_on = 1;
    snapshot.brightness = 255;
    snapshot.seal();
    return snapshot;
}

void only_a_sealed_snapshot_is_valid() {
    // Memory after a power cycle: zeroes, or anything at all.
    RunSnapshot blank;
    assert(!blank.valid());
    RunSnapshot noise;
    std::memset(static_cast<void*>(&noise), 0xA5, sizeof(noise));
    assert(!noise.valid());

    // Filled in but not sealed: the restart came in the middle of a write.
    RunSnapshot unsealed;
    unsealed.uptime_ms = 5000U;
    unsealed.cpu_mhz = 240;
    assert(!unsealed.valid());

    assert(sample().valid());
}

void a_changed_field_is_noticed() {
    RunSnapshot changed = sample();
    changed.uptime_ms += 1U;
    assert(!changed.valid());
    changed = sample(); changed.uptime_ms ^= 0x00010000U; assert(!changed.valid());
    changed = sample(); changed.cpu_mhz = 80; assert(!changed.valid());
    changed = sample(); changed.grind_phase = 10; assert(!changed.valid());
    changed = sample(); changed.motor_on = 0; assert(!changed.valid());
    changed = sample(); changed.wifi_state = 4; assert(!changed.valid());
    changed = sample(); changed.display_on = 0; assert(!changed.valid());
    changed = sample(); changed.brightness = 51; assert(!changed.valid());
    changed = sample(); changed.magic = 0; assert(!changed.valid());

    // Sealing again after a change makes it valid again.
    changed = sample();
    changed.grind_phase = 2;
    changed.seal();
    assert(changed.valid());

    // Two fields swapped do not cancel out.
    changed = sample();
    changed.motor_on = 0;
    changed.display_on = 0;
    changed.grind_phase = static_cast<uint8_t>(changed.grind_phase + 10);
    assert(!changed.valid());
}

}  // namespace

int main() {
    only_a_sealed_snapshot_is_valid();
    a_changed_field_is_noticed();
    return 0;
}
