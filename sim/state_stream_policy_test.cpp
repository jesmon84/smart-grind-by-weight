// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cstdint>

#include "network/state_stream_policy.h"

namespace {

void during_a_grind_every_message_goes_out() {
    // The caller asks every kActiveStateIntervalMs; a grind is sent each time.
    assert(state_message_due(kActiveStateIntervalMs, true, false, false));
    assert(state_message_due(0U, true, false, false));

    // The start and the end of a grind are sent at once as well.
    assert(state_message_due(0U, false, true, false));
    assert(state_message_due(kActiveStateIntervalMs, false, true, false));
}

void an_idle_grinder_sends_changes_and_keepalives() {
    // Nothing changed: quiet until the keep-alive.
    assert(!state_message_due(kIdleStateIntervalMs, false, false, false));
    assert(!state_message_due(kIdleKeepaliveIntervalMs - 1U, false, false, false));
    assert(state_message_due(kIdleKeepaliveIntervalMs, false, false, false));
    assert(state_message_due(10U * kIdleKeepaliveIntervalMs, false, false, false));

    // A change goes out, but not closer to the last message than the idle
    // interval.
    assert(!state_message_due(0U, false, false, true));
    assert(!state_message_due(kIdleStateIntervalMs - 1U, false, false, true));
    assert(state_message_due(kIdleStateIntervalMs, false, false, true));
    assert(state_message_due(kIdleKeepaliveIntervalMs, false, false, true));

    // The keep-alive is rarer than the changes, and the idle rate is slower
    // than the rate during a grind.
    static_assert(kIdleKeepaliveIntervalMs > kIdleStateIntervalMs);
    static_assert(kIdleStateIntervalMs > kActiveStateIntervalMs);
}

void what_counts_as_a_change() {
    LiveState sent;
    sent.phase = "IDLE";
    sent.mode = 0;
    sent.profile = 1;
    sent.target_weight = 18.0f;
    sent.target_time_ms = 10600U;
    sent.weight = 0.02f;
    sent.flow = 0.0f;

    LiveState now = sent;
    assert(!live_state_changed(sent, now));

    // The same phase name held in another place is still the same phase.
    const char idle_copy[] = "IDLE";
    now.phase = idle_copy;
    assert(!live_state_changed(sent, now));
    now.phase = "COMPLETED";
    assert(live_state_changed(sent, now));

    // The scale's resting wander is not a change; a real one is, both ways.
    now = sent;
    now.weight = sent.weight + 0.03f;
    assert(!live_state_changed(sent, now));
    now.weight = sent.weight - 0.04f;
    assert(!live_state_changed(sent, now));
    now.weight = sent.weight + kLiveWeightStepG;
    assert(live_state_changed(sent, now));
    now.weight = sent.weight - 2.0f * kLiveWeightStepG;
    assert(live_state_changed(sent, now));

    // Small steps add up against the value that was sent, not the last one
    // looked at, so a slow drift is still reported.
    now = sent;
    for (int step = 0; step < 3; ++step) {
        now.weight += 0.02f;
    }
    assert(live_state_changed(sent, now));

    now = sent;
    now.flow = sent.flow + 0.5f * kLiveFlowStepGPerS;
    assert(!live_state_changed(sent, now));
    now.flow = sent.flow + kLiveFlowStepGPerS;
    assert(live_state_changed(sent, now));

    // Everything else a client shows counts at once.
    now = sent; now.active = true; assert(live_state_changed(sent, now));
    now = sent; now.mode = 1; assert(live_state_changed(sent, now));
    now = sent; now.profile = 2; assert(live_state_changed(sent, now));
    now = sent; now.progress = 1; assert(live_state_changed(sent, now));
    now = sent; now.target_weight = 18.1f; assert(live_state_changed(sent, now));
    now = sent; now.target_time_ms = 10700U; assert(live_state_changed(sent, now));
    now = sent; now.motor_running = true; assert(live_state_changed(sent, now));
}

}  // namespace

int main() {
    during_a_grind_every_message_goes_out();
    an_idle_grinder_sends_changes_and_keepalives();
    what_counts_as_a_change();
    return 0;
}
