// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cstdint>

#include "system/profile_settle_policy.h"

int main() {
    ProfileSettleTracker tracker;

    // Nothing to store until a profile page comes into view.
    assert(!tracker.pending());
    assert(!tracker.settled(100000U));

    // A profile that stays in view is stored once, after the settle time.
    tracker.profile_shown(1000U);
    assert(tracker.pending());
    assert(!tracker.settled(1000U + kProfileSettleMs - 1U));
    assert(tracker.settled(1000U + kProfileSettleMs));
    assert(!tracker.pending());
    assert(!tracker.settled(1000U + 10U * kProfileSettleMs));

    // Swiping on to the next profile restarts the wait.
    tracker.profile_shown(5000U);
    tracker.profile_shown(5000U + kProfileSettleMs - 1U);
    assert(!tracker.settled(5000U + kProfileSettleMs));
    assert(tracker.settled(5000U + 2U * kProfileSettleMs - 1U));

    // Passing over a profile on the way to the menu stores nothing.
    tracker.profile_shown(20000U);
    tracker.cancel();
    assert(!tracker.pending());
    assert(!tracker.settled(20000U + 10U * kProfileSettleMs));

    // The millisecond clock wrapping does not store early or never.
    const uint32_t before_wrap = UINT32_MAX - 500U;
    tracker.profile_shown(before_wrap);
    assert(!tracker.settled(before_wrap + kProfileSettleMs - 1U));
    assert(tracker.settled(before_wrap + kProfileSettleMs));
    return 0;
}
