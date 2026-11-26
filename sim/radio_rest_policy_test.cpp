// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cstdint>

#include "network/radio_rest_policy.h"

namespace {

void radio_rests_through_a_grind() {
    RadioRestSchedule schedule;
    uint32_t now = 100000U;

    // Idle: the radio stays on.
    assert(!schedule.update(now, true, false));
    assert(!schedule.update(now + 60000U, true, false));

    // Off as soon as a grind starts, and for all of it.
    now += 60000U;
    for (uint32_t elapsed = 0; elapsed <= 30000U; elapsed += 500U) {
        assert(schedule.update(now + elapsed, true, true));
    }
    now += 30000U;

    // Still off just after the grind, back on once the delay has passed.
    assert(schedule.update(now + 1U, true, false));
    assert(schedule.update(now + kRadioResumeDelayMs - 1U, true, false));
    assert(!schedule.update(now + kRadioResumeDelayMs, true, false));
    assert(!schedule.update(now + 10U * kRadioResumeDelayMs, true, false));
}

void a_second_grind_or_a_pulse_keeps_it_off() {
    RadioRestSchedule schedule;
    uint32_t now = 5000U;
    assert(schedule.update(now, true, true));

    // Another grind, or a pulse on the finished screen, before the radio is
    // back: it stays off, and the delay counts from the end of that one.
    now += 10000U;
    assert(schedule.update(now, true, false));
    assert(schedule.update(now + kRadioResumeDelayMs - 1000U, true, true));
    now += kRadioResumeDelayMs - 1000U;
    assert(schedule.update(now + kRadioResumeDelayMs - 1U, true, false));
    assert(!schedule.update(now + kRadioResumeDelayMs, true, false));
}

void switched_off_it_never_rests() {
    RadioRestSchedule schedule;
    assert(!schedule.update(1000U, false, true));
    assert(!schedule.update(2000U, false, false));

    // Switching the rest off in the middle of one brings the radio back at
    // once, and switching it on again does not rest it without a grind.
    assert(schedule.update(3000U, true, true));
    assert(!schedule.update(3100U, false, true));
    assert(!schedule.update(3200U, true, false));
}

void the_clock_wrapping_does_not_strand_the_radio() {
    RadioRestSchedule schedule;
    const uint32_t before_wrap = UINT32_MAX - 1000U;
    assert(schedule.update(before_wrap, true, true));
    assert(schedule.update(before_wrap + kRadioResumeDelayMs - 1U, true, false));
    assert(!schedule.update(before_wrap + kRadioResumeDelayMs, true, false));

    // The delay is long enough for the result to be saved and short enough
    // that the web page is back before anyone has walked to it.
    static_assert(kRadioResumeDelayMs >= 5000U);
    static_assert(kRadioResumeDelayMs <= 60000U);
}

}  // namespace

int main() {
    radio_rests_through_a_grind();
    a_second_grind_or_a_pulse_keeps_it_off();
    switched_off_it_never_rests();
    the_clock_wrapping_does_not_strand_the_radio();
    return 0;
}
