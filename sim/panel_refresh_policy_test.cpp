// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <initializer_list>

#include "hardware/panel_refresh_policy.h"

namespace {

// Runs one whole refresh starting at `now` and returns when it finished.
uint32_t run_refresh(PanelRefreshSchedule& schedule, uint32_t now) {
    assert(schedule.next_step(now, true) == PanelRefreshStep::SLEEP_OUT);
    assert(schedule.settling());
    assert(schedule.next_step(now + kPanelSleepOutSettleMs - 1U, true) == PanelRefreshStep::NONE);
    now += kPanelSleepOutSettleMs;
    assert(schedule.next_step(now, true) == PanelRefreshStep::DISPLAY_ON);
    assert(!schedule.settling());
    return now;
}

void resend_schedule() {
    // Nothing is sent before the panel has been set up.
    PanelRefreshSchedule idle;
    idle.request();
    assert(idle.next_step(60000U, true) == PanelRefreshStep::NONE);

    // The setup is resent every interval, counted from the end of the last one.
    PanelRefreshSchedule schedule;
    uint32_t now = 1000U;
    schedule.start(now);
    assert(schedule.next_step(now, true) == PanelRefreshStep::NONE);
    assert(schedule.next_step(now + kPanelRefreshIntervalMs - 1U, true) == PanelRefreshStep::NONE);
    now = run_refresh(schedule, now + kPanelRefreshIntervalMs);
    assert(schedule.next_step(now + kPanelRefreshIntervalMs - 1U, true) == PanelRefreshStep::NONE);
    now = run_refresh(schedule, now + kPanelRefreshIntervalMs);

    // A touch refreshes without waiting for the interval, but repeated
    // touches cannot run refreshes back to back.
    schedule.request();
    assert(schedule.next_step(now, true) == PanelRefreshStep::NONE);
    const uint32_t earliest = now - kPanelSleepOutSettleMs + kPanelRefreshMinSpacingMs;
    assert(schedule.next_step(earliest - 1U, true) == PanelRefreshStep::NONE);
    now = run_refresh(schedule, earliest);
    // One request gives one refresh.
    assert(schedule.next_step(now + kPanelRefreshMinSpacingMs, true) == PanelRefreshStep::NONE);

    // A request made while a refresh is settling runs after it.
    assert(schedule.next_step(now + kPanelRefreshIntervalMs, true) == PanelRefreshStep::SLEEP_OUT);
    now += kPanelRefreshIntervalMs;
    schedule.request();
    assert(schedule.next_step(now + kPanelSleepOutSettleMs, true) == PanelRefreshStep::DISPLAY_ON);
    now += kPanelSleepOutSettleMs;
    assert(schedule.next_step(now, true) == PanelRefreshStep::NONE);
    now = run_refresh(schedule, now - kPanelSleepOutSettleMs + kPanelRefreshMinSpacingMs);

    // A panel switched off on purpose is left alone, however long it is off,
    // and an unfinished refresh is dropped.
    assert(schedule.next_step(now + kPanelRefreshIntervalMs, true) == PanelRefreshStep::SLEEP_OUT);
    now += kPanelRefreshIntervalMs;
    assert(schedule.next_step(now + 10U, false) == PanelRefreshStep::NONE);
    assert(!schedule.settling());
    assert(schedule.next_step(now + 10U * kPanelRefreshIntervalMs, false) == PanelRefreshStep::NONE);

    // A request made on waking runs at once after a long sleep.
    now += 10U * kPanelRefreshIntervalMs;
    schedule.request();
    now = run_refresh(schedule, now);

    // The millisecond clock wrapping does not stall or hurry the schedule.
    PanelRefreshSchedule wrapping;
    const uint32_t before_wrap = UINT32_MAX - 1000U;
    wrapping.start(before_wrap);
    assert(wrapping.next_step(before_wrap + kPanelRefreshIntervalMs - 1U, true) ==
           PanelRefreshStep::NONE);
    run_refresh(wrapping, before_wrap + kPanelRefreshIntervalMs);
}

void frame_signal_on_a_board_without_it() {
    PanelFrameWatch watch;
    // Not judged before the panel has been set up.
    assert(watch.update(60000U, 0U, true) == PanelFrameState::UNKNOWN);

    watch.panel_set_up(1000U);
    assert(watch.update(1000U + kPanelFrameAbsentAfterMs - 1U, 0U, true) == PanelFrameState::UNKNOWN);
    assert(watch.update(1000U + kPanelFrameAbsentAfterMs, 0U, true) == PanelFrameState::ABSENT);

    // It is never taken for lost, whatever happens to the panel afterwards.
    assert(watch.update(1000U + 100U * kPanelFrameLostAfterMs, 0U, true) == PanelFrameState::ABSENT);
    watch.panel_set_up(900000U);
    assert(watch.state() == PanelFrameState::ABSENT);
    assert(watch.update(900000U + 100U, 0U, true) == PanelFrameState::ABSENT);
    assert(watch.update(900000U + 10U * kPanelFrameLostAfterMs, 0U, true) == PanelFrameState::ABSENT);
    assert(watch.update(920000U, 0U, false) == PanelFrameState::ABSENT);
    assert(watch.update(940000U, 0U, true) == PanelFrameState::ABSENT);

    // A signal that turns up after all is followed from then on.
    assert(watch.update(950000U, 1U, true) == PanelFrameState::RUNNING);
    assert(watch.update(950000U + kPanelFrameLostAfterMs, 1U, true) == PanelFrameState::LOST);
}

void frame_signal_stopping_and_returning() {
    PanelFrameWatch watch;
    uint32_t now = 5000U;
    uint32_t pulses = 0;
    watch.panel_set_up(now);
    assert(watch.state() == PanelFrameState::UNKNOWN);

    // Pulses arriving: running.
    for (int cycle = 0; cycle < 10; ++cycle) {
        now += 16U;
        ++pulses;
        assert(watch.update(now, pulses, true) == PanelFrameState::RUNNING);
    }

    // A short gap is not a loss; a full interval without a pulse is.
    assert(watch.update(now + kPanelFrameLostAfterMs - 1U, pulses, true) == PanelFrameState::RUNNING);
    assert(watch.update(now + kPanelFrameLostAfterMs, pulses, true) == PanelFrameState::LOST);
    now += kPanelFrameLostAfterMs;
    assert(watch.update(now + 60000U, pulses, true) == PanelFrameState::LOST);

    // Pulses again: running again.
    now += 60000U;
    assert(watch.update(now, ++pulses, true) == PanelFrameState::RUNNING);

    // A panel that is off on purpose is asleep and sends no pulses. It is
    // not judged, however long it is off, and reads as unknown.
    assert(watch.update(now + 500U, pulses, false) == PanelFrameState::UNKNOWN);
    now += 3600000U;
    assert(watch.update(now, pulses, false) == PanelFrameState::UNKNOWN);
    // Back on and pulsing: running.
    assert(watch.update(now + 100U, ++pulses, true) == PanelFrameState::RUNNING);
    // Off again and back on without a pulse: it has a whole interval to
    // start pulsing in, and is lost after that.
    now += 60000U;
    assert(watch.update(now, pulses, false) == PanelFrameState::UNKNOWN);
    assert(watch.update(now + kPanelFrameLostAfterMs - 1U, pulses, true) == PanelFrameState::UNKNOWN);
    assert(watch.update(now + kPanelFrameLostAfterMs, pulses, true) == PanelFrameState::LOST);
    now += kPanelFrameLostAfterMs;

    // After a reset the signal is not judged until it has had its interval.
    watch.panel_set_up(now);
    assert(watch.state() == PanelFrameState::UNKNOWN);
    assert(watch.update(now + kPanelFrameLostAfterMs - 1U, pulses, true) == PanelFrameState::UNKNOWN);
    assert(watch.update(now + kPanelFrameLostAfterMs, pulses, true) == PanelFrameState::LOST);
    watch.panel_set_up(now + 2000U);
    assert(watch.update(now + 2100U, ++pulses, true) == PanelFrameState::RUNNING);

    // The millisecond clock wrapping does not lose or invent a signal.
    PanelFrameWatch wrapping;
    const uint32_t before_wrap = UINT32_MAX - 300U;
    wrapping.panel_set_up(before_wrap);
    assert(wrapping.update(before_wrap + 100U, 1U, true) == PanelFrameState::RUNNING);
    assert(wrapping.update(before_wrap + 100U + kPanelFrameLostAfterMs - 1U, 1U, true) ==
           PanelFrameState::RUNNING);
    assert(wrapping.update(before_wrap + 100U + kPanelFrameLostAfterMs, 1U, true) ==
           PanelFrameState::LOST);
}

// Runs the reset that is due at `now` and returns when it finished.
uint32_t run_reset(PanelResetSchedule& schedule, uint32_t now, PanelResetCause expected) {
    assert(schedule.due(now, true, true));
    assert(schedule.begin() == expected);
    assert(!schedule.pending());
    now += 260U;  // The reset pulse and the setup block the UI task this long
    schedule.finished(now);
    return now;
}

void reset_waits_for_a_quiet_screen() {
    PanelResetSchedule schedule;
    uint32_t now = 10000U;
    assert(!schedule.due(now, true, true));

    // Nobody touching: the reset runs at once, and once.
    schedule.request(PanelResetCause::REQUESTED, now);
    assert(schedule.pending());
    now = run_reset(schedule, now, PanelResetCause::REQUESTED);
    assert(!schedule.due(now + 60000U, true, true));

    // A finger on the screen holds it off until the screen is left alone.
    now += 60000U;
    schedule.request(PanelResetCause::FRAME_LOST, now);
    assert(!schedule.due(now, true, false));
    assert(!schedule.due(now + kPanelResetMaxWaitMs - 1U, true, false));
    now = run_reset(schedule, now + 4000U, PanelResetCause::FRAME_LOST);

    // A contact that never lets go cannot hold it off for good.
    now += 60000U;
    schedule.request(PanelResetCause::REQUESTED, now);
    assert(!schedule.due(now + kPanelResetMaxWaitMs - 1U, true, false));
    assert(schedule.due(now + kPanelResetMaxWaitMs, true, false));
    assert(schedule.begin() == PanelResetCause::REQUESTED);
    now += kPanelResetMaxWaitMs + 260U;
    schedule.finished(now);

    // Two causes waiting are served by one reset, put down to the first.
    now += 60000U;
    schedule.request(PanelResetCause::FRAME_LOST, now);
    schedule.request(PanelResetCause::REQUESTED, now + 100U);
    now = run_reset(schedule, now + 100U, PanelResetCause::FRAME_LOST);
    assert(!schedule.due(now + 60000U, true, true));
    schedule.request(PanelResetCause::REQUESTED, now + 60000U);
    schedule.request(PanelResetCause::FRAME_LOST, now + 60100U);
    now = run_reset(schedule, now + 60100U, PanelResetCause::REQUESTED);

    // Resets do not run back to back.
    schedule.request(PanelResetCause::REQUESTED, now);
    assert(!schedule.due(now + kPanelResetMinSpacingMs - 1U, true, true));
    now = run_reset(schedule, now + kPanelResetMinSpacingMs, PanelResetCause::REQUESTED);

    // A panel that is off on purpose is not reset here: switching it off
    // resets it. A request made before it went off does not run when it
    // comes back on.
    now += 60000U;
    schedule.request(PanelResetCause::REQUESTED, now);
    assert(!schedule.due(now, false, true));
    assert(!schedule.pending());
    assert(!schedule.due(now + 5000U, true, true));

    // The millisecond clock wrapping does not stall or hurry a reset.
    PanelResetSchedule wrapping;
    const uint32_t before_wrap = UINT32_MAX - 500U;
    wrapping.request(PanelResetCause::REQUESTED, before_wrap);
    assert(!wrapping.due(before_wrap + 1000U, true, false));
    uint32_t wrapped = run_reset(wrapping, before_wrap + 1000U, PanelResetCause::REQUESTED);
    wrapping.request(PanelResetCause::REQUESTED, wrapped);
    assert(!wrapping.due(wrapped + kPanelResetMinSpacingMs - 1U, true, true));
    assert(wrapping.due(wrapped + kPanelResetMinSpacingMs, true, true));
}

void lost_frame_signal_gets_a_resend_then_resets() {
    PanelResetSchedule schedule;
    uint32_t now = 20000U;

    // A signal that is running, unknown or absent asks for nothing.
    for (PanelFrameState state : {PanelFrameState::RUNNING, PanelFrameState::UNKNOWN,
                                  PanelFrameState::ABSENT}) {
        const PanelFrameEvents events = schedule.follow_frame(now, state, true);
        assert(!events.lost && !events.recovered);
        assert(!schedule.pending());
    }

    // Lost: a resend at once, and only once.
    PanelFrameEvents events = schedule.follow_frame(now, PanelFrameState::LOST, true);
    assert(events.lost && !events.recovered);
    assert(!schedule.pending());
    events = schedule.follow_frame(now + 16U, PanelFrameState::LOST, true);
    assert(!events.lost && !events.recovered);

    // The resend brought it back: no reset.
    events = schedule.follow_frame(now + 300U, PanelFrameState::RUNNING, true);
    assert(events.recovered && !events.lost);
    events = schedule.follow_frame(now + 316U, PanelFrameState::RUNNING, true);
    assert(!events.recovered);
    assert(!schedule.pending());

    // Lost and not brought back by the resend: a reset, after the wait.
    now += 60000U;
    assert(schedule.follow_frame(now, PanelFrameState::LOST, true).lost);
    schedule.follow_frame(now + kPanelFrameResetAfterMs - 1U, PanelFrameState::LOST, true);
    assert(!schedule.pending());
    schedule.follow_frame(now + kPanelFrameResetAfterMs, PanelFrameState::LOST, true);
    assert(schedule.pending());
    now = run_reset(schedule, now + kPanelFrameResetAfterMs, PanelResetCause::FRAME_LOST);

    // While the panel has not been judged again nothing more is asked for.
    events = schedule.follow_frame(now + 500U, PanelFrameState::UNKNOWN, true);
    assert(!events.lost && !events.recovered && !schedule.pending());

    // Still lost: more resets, each after the wait, up to the limit.
    uint8_t resets = 1;
    for (int second = 0; second < 60; ++second) {
        now += 1000U;
        events = schedule.follow_frame(now, PanelFrameState::LOST, true);
        assert(!events.lost);  // One loss, however long it lasts
        if (schedule.due(now, true, true)) {
            assert(schedule.begin() == PanelResetCause::FRAME_LOST);
            schedule.finished(now);
            ++resets;
        }
    }
    assert(resets == kPanelFrameResetAttempts);
    assert(!schedule.pending());

    // A reset for another cause still runs, and when the signal is back the
    // loss is over: the next one gets its own resend and resets.
    schedule.request(PanelResetCause::REQUESTED, now);
    now = run_reset(schedule, now + kPanelResetMinSpacingMs, PanelResetCause::REQUESTED);
    assert(schedule.follow_frame(now + 100U, PanelFrameState::RUNNING, true).recovered);
    now += 60000U;
    assert(schedule.follow_frame(now, PanelFrameState::LOST, true).lost);
    schedule.follow_frame(now + kPanelFrameResetAfterMs, PanelFrameState::LOST, true);
    assert(schedule.pending());
    now = run_reset(schedule, now + kPanelFrameResetAfterMs, PanelResetCause::FRAME_LOST);
    assert(schedule.follow_frame(now + 100U, PanelFrameState::RUNNING, true).recovered);

    // A reset already waiting serves the lost signal too, without using up
    // one of its attempts.
    now += 60000U;
    schedule.request(PanelResetCause::REQUESTED, now);
    assert(schedule.follow_frame(now, PanelFrameState::LOST, true).lost);
    schedule.follow_frame(now + kPanelFrameResetAfterMs, PanelFrameState::LOST, true);
    now = run_reset(schedule, now + kPanelFrameResetAfterMs, PanelResetCause::REQUESTED);
    resets = 0;
    for (int second = 0; second < 60; ++second) {
        now += 1000U;
        schedule.follow_frame(now, PanelFrameState::LOST, true);
        if (schedule.due(now, true, true)) {
            assert(schedule.begin() == PanelResetCause::FRAME_LOST);
            schedule.finished(now);
            ++resets;
        }
    }
    assert(resets == kPanelFrameResetAttempts);

    // Switching the display off and on resets the panel as well. A signal
    // that runs after that counts as the recovery.
    events = schedule.follow_frame(now + 1000U, PanelFrameState::UNKNOWN, false);
    assert(!events.lost && !events.recovered && !schedule.pending());
    events = schedule.follow_frame(now + 5000U, PanelFrameState::RUNNING, true);
    assert(events.recovered && !events.lost);

    // A panel that is off on purpose is not reset for a signal that stopped.
    PanelResetSchedule off;
    events = off.follow_frame(1000U, PanelFrameState::LOST, false);
    assert(!events.lost && !off.pending());
    off.follow_frame(1000U + 10U * kPanelFrameResetAfterMs, PanelFrameState::LOST, false);
    assert(!off.pending());
}

void picture_is_shown_once_the_screen_is_drawn() {
    // Nothing to show on a panel that was not set up, drawn to or not.
    PanelReveal reveal;
    assert(!reveal.hidden());
    assert(!reveal.due(1000U));
    reveal.screen_drawn();
    assert(!reveal.due(60000U));

    // Dark until the screen has been drawn; then shown, once.
    uint32_t now = 5000U;
    reveal.hide(now);
    assert(reveal.hidden());
    assert(!reveal.due(now));
    assert(!reveal.due(now + kPanelRevealTimeoutMs - 1U));
    reveal.screen_drawn();
    assert(reveal.hidden());
    assert(reveal.due(now + 20U));
    assert(!reveal.hidden());
    assert(!reveal.due(now + 21U));
    assert(!reveal.due(now + 10U * kPanelRevealTimeoutMs));

    // A drawing made before the set-up does not count for it.
    reveal.screen_drawn();
    now += 60000U;
    reveal.hide(now);
    assert(!reveal.due(now + 1U));

    // No drawing at all: shown when the wait is over, once.
    assert(!reveal.due(now + kPanelRevealTimeoutMs - 1U));
    assert(reveal.due(now + kPanelRevealTimeoutMs));
    assert(!reveal.due(now + kPanelRevealTimeoutMs + 1U));

    // Set up again before the picture was shown: the wait starts again, and
    // the drawing from before does not count.
    now += 60000U;
    reveal.hide(now);
    reveal.screen_drawn();
    reveal.hide(now + 100U);
    assert(!reveal.due(now + 101U));
    assert(!reveal.due(now + 100U + kPanelRevealTimeoutMs - 1U));
    assert(reveal.due(now + 100U + kPanelRevealTimeoutMs));

    // The millisecond clock wrapping does not hold the picture back or show
    // it early.
    PanelReveal wrapping;
    const uint32_t before_wrap = UINT32_MAX - 100U;
    wrapping.hide(before_wrap);
    assert(!wrapping.due(before_wrap + kPanelRevealTimeoutMs - 1U));
    assert(wrapping.due(before_wrap + kPanelRevealTimeoutMs));
}

void names_for_the_status_report() {
    constexpr auto same = [](const char* a, const char* b) {
        while (*a && *a == *b) { ++a; ++b; }
        return *a == *b;
    };
    static_assert(same(panel_frame_state_name(PanelFrameState::RUNNING), "running"));
    static_assert(same(panel_frame_state_name(PanelFrameState::LOST), "lost"));
    static_assert(same(panel_frame_state_name(PanelFrameState::ABSENT), "absent"));
    static_assert(same(panel_frame_state_name(PanelFrameState::UNKNOWN), "unknown"));
    static_assert(same(panel_reset_cause_name(PanelResetCause::FRAME_LOST), "frame signal lost"));
    static_assert(same(panel_reset_cause_name(PanelResetCause::REQUESTED), "requested"));
}

}  // namespace

int main() {
    resend_schedule();
    frame_signal_on_a_board_without_it();
    frame_signal_stopping_and_returning();
    reset_waits_for_a_quiet_screen();
    lost_frame_signal_gets_a_resend_then_resets();
    picture_is_shown_once_the_screen_is_drawn();
    names_for_the_status_report();
    return 0;
}
