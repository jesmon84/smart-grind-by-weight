// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cstdint>

#include "system/power_saving_policy.h"

namespace {

void processor_clock() {
    // Slow only when nothing needs the speed.
    assert(cpu_clock_mhz(false, false, false) == kCpuMhzResting);

    // The display in use, a grind or a Bluetooth transfer each need full speed.
    assert(cpu_clock_mhz(true, false, false) == kCpuMhzInUse);
    assert(cpu_clock_mhz(false, true, false) == kCpuMhzInUse);
    assert(cpu_clock_mhz(false, false, true) == kCpuMhzInUse);
    assert(cpu_clock_mhz(true, true, true) == kCpuMhzInUse);
    // Firmware arriving over Bluetooth runs slowly, whatever else is going on.
    assert(cpu_clock_mhz(true, false, true, true) == kCpuMhzResting);
    assert(cpu_clock_mhz(true, false, true, false) == kCpuMhzInUse);

    // The update screen during an upload: a third of the normal brightness,
    // never below the lowest the display allows, never above normal.
    assert(upload_brightness(1.0f, 0.1f) > 0.32f && upload_brightness(1.0f, 0.1f) < 0.34f);
    assert(upload_brightness(0.15f, 0.1f) == 0.1f);
    assert(upload_brightness(0.05f, 0.1f) == 0.05f);

    // The radio does not work below 80 MHz.
    static_assert(kCpuMhzResting >= 80U);
    static_assert(kCpuMhzResting < kCpuMhzInUse);
}

void first_update_check() {
    constexpr uint32_t never = 0U;

    // Not soon after a start, however quiet the grinder is.
    assert(!update_check_due(1U, never, false, true));
    assert(!update_check_due(120000U, never, false, true));
    assert(!update_check_due(kFirstUpdateCheckDelayMs - 1U, never, false, true));
    assert(update_check_due(kFirstUpdateCheckDelayMs, never, false, true));

    // Not while the display is in use, however long the grinder has run.
    assert(!update_check_due(kFirstUpdateCheckDelayMs, never, false, false));
    assert(!update_check_due(10U * kFirstUpdateCheckDelayMs, never, false, false));
    assert(update_check_due(10U * kFirstUpdateCheckDelayMs, never, false, true));

    // The first check waits much longer than the time a start and a page
    // reload after a firmware upload take.
    static_assert(kFirstUpdateCheckDelayMs >= 10U * 60U * 1000U);
}

void later_update_checks() {
    const uint32_t checked = 40U * 60U * 1000U;

    // Once a day after a check that worked.
    assert(!update_check_due(checked + 1U, checked, false, true));
    assert(!update_check_due(checked + kUpdateCheckIntervalMs - 1U, checked, false, true));
    assert(update_check_due(checked + kUpdateCheckIntervalMs, checked, false, true));

    // Sooner after one that failed, but not for hours.
    assert(!update_check_due(checked + kUpdateCheckRetryMs - 1U, checked, true, true));
    assert(update_check_due(checked + kUpdateCheckRetryMs, checked, true, true));
    static_assert(kUpdateCheckRetryMs < kUpdateCheckIntervalMs);
    static_assert(kUpdateCheckRetryMs >= 60U * 60U * 1000U);

    // A check that is due waits for the display to rest.
    assert(!update_check_due(checked + 2U * kUpdateCheckIntervalMs, checked, false, false));
    assert(update_check_due(checked + 2U * kUpdateCheckIntervalMs, checked, false, true));

    // The millisecond clock wrapping does not stall or hurry a check.
    const uint32_t before_wrap = UINT32_MAX - 1000U;
    assert(!update_check_due(before_wrap + kUpdateCheckRetryMs - 1U, before_wrap, true, true));
    assert(update_check_due(before_wrap + kUpdateCheckRetryMs, before_wrap, true, true));
}

}  // namespace

int main() {
    processor_clock();
    first_update_check();
    later_update_checks();
    return 0;
}
