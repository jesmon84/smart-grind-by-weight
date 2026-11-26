#pragma once

#include <cstdint>

// The AMOLED controller can lose its setup, or stop showing a picture
// altogether, to a supply dip or to motor switching noise while the ESP32
// keeps running. Nothing reports that, so the screen would stay black until a
// power cycle. Three things bring it back:
//
// - A resend of the setup, which shows no change on a healthy panel. It is
//   sent on a schedule while the panel should be on, and without waiting when
//   the panel is touched. It only helps a panel that restarted cleanly.
// - Switching the display off puts the panel to sleep and resets it, and
//   switching it on sets it up from scratch, as at power-on. That clears
//   every state the panel can be in, where nobody sees it happen.
// - A full reset while the display is on: the reset pin is pulsed and the
//   panel set up from scratch. The picture goes black for about a quarter of
//   a second, so this is for a panel in trouble only (see PanelResetCause),
//   never a precaution.

//------------------------------------------------------------------------------
// Resend
//------------------------------------------------------------------------------

// Longest a cleanly restarted panel stays dark before its setup is resent.
constexpr uint32_t kPanelRefreshIntervalMs = 3000U;
// Closest spacing of refreshes asked for by a touch or a lost frame signal,
// so repeated taps cannot run them back to back.
constexpr uint32_t kPanelRefreshMinSpacingMs = 1000U;
// The controller takes no other command for 5 ms after Sleep Out; the pause
// is rounded to whole ticks, so it is set a little longer.
constexpr uint32_t kPanelSleepOutQuietMs = 8U;
// Time a controller that really was asleep needs before Display On.
constexpr uint32_t kPanelSleepOutSettleMs = 100U;

enum class PanelRefreshStep : uint8_t {
    NONE,
    SLEEP_OUT,   // Send Sleep Out, then the register setup
    DISPLAY_ON,  // Send Display On and the brightness, then redraw
};

// Decides when each half of a refresh is due. One refresh is Sleep Out with
// the register setup, followed kPanelSleepOutSettleMs later by Display On.
class PanelRefreshSchedule {
public:
    // Call once the panel has been set up; the first interval starts here.
    void start(uint32_t now_ms) {
        started_ = true;
        settling_ = false;
        requested_ = false;
        last_started_ms_ = now_ms;
        last_finished_ms_ = now_ms;
    }

    // Asks for a refresh as soon as the minimum spacing allows.
    void request() { requested_ = true; }

    bool settling() const { return settling_; }

    PanelRefreshStep next_step(uint32_t now_ms, bool panel_should_be_on) {
        if (!started_) {
            return PanelRefreshStep::NONE;
        }
        if (!panel_should_be_on) {
            // A panel that is off on purpose is asleep and left alone. Waking
            // sets it up from scratch, so an unfinished refresh is dropped.
            settling_ = false;
            return PanelRefreshStep::NONE;
        }

        if (settling_) {
            if (now_ms - last_started_ms_ < kPanelSleepOutSettleMs) {
                return PanelRefreshStep::NONE;
            }
            settling_ = false;
            last_finished_ms_ = now_ms;
            return PanelRefreshStep::DISPLAY_ON;
        }

        const bool due = requested_
            ? now_ms - last_started_ms_ >= kPanelRefreshMinSpacingMs
            : now_ms - last_finished_ms_ >= kPanelRefreshIntervalMs;
        if (!due) {
            return PanelRefreshStep::NONE;
        }
        requested_ = false;
        settling_ = true;
        last_started_ms_ = now_ms;
        return PanelRefreshStep::SLEEP_OUT;
    }

private:
    bool started_ = false;
    bool settling_ = false;
    bool requested_ = false;
    uint32_t last_started_ms_ = 0;
    uint32_t last_finished_ms_ = 0;
};

//------------------------------------------------------------------------------
// Frame signal
//------------------------------------------------------------------------------
// A running panel pulses its frame (tearing effect) line about 60 times a
// second. The line goes quiet when the controller has restarted or fallen
// asleep. A panel can also go dark with the line still pulsing, so a running
// signal does not prove there is a picture.

// No pulse for this long: the panel has stopped.
constexpr uint32_t kPanelFrameLostAfterMs = 1000U;
// No pulse at all this long after the first setup: this board does not carry
// the signal, and it is never used to judge the panel.
constexpr uint32_t kPanelFrameAbsentAfterMs = 3000U;

enum class PanelFrameState : uint8_t {
    UNKNOWN,  // Just set up; not judged yet
    ABSENT,   // Never seen on this board
    RUNNING,
    LOST,     // Was running and stopped while the panel should be on
};

constexpr const char* panel_frame_state_name(PanelFrameState state) {
    return state == PanelFrameState::RUNNING ? "running"
         : state == PanelFrameState::LOST ? "lost"
         : state == PanelFrameState::ABSENT ? "absent"
         : "unknown";
}

// Follows the count of frame pulses.
class PanelFrameWatch {
public:
    // Call when the panel has been set up: at power-on, on waking and after
    // a reset. The signal gets a fresh interval to appear in.
    void panel_set_up(uint32_t now_ms) {
        if (!started_) {
            started_ = true;
            first_set_up_ms_ = now_ms;
        }
        last_pulse_ms_ = now_ms;
        if (seen_) {
            state_ = PanelFrameState::UNKNOWN;
        }
    }

    // `pulse_count` is the running total of pulses. Call every UI cycle.
    PanelFrameState update(uint32_t now_ms, uint32_t pulse_count, bool panel_should_be_on) {
        if (!started_) {
            return state_;
        }
        const bool pulsed = pulse_count != last_pulse_count_;
        last_pulse_count_ = pulse_count;
        if (pulsed) {
            seen_ = true;
            last_pulse_ms_ = now_ms;
            state_ = PanelFrameState::RUNNING;
            return state_;
        }
        if (!panel_should_be_on) {
            // A panel that is off on purpose is asleep and sends no pulses.
            // It is not judged, and gets a fresh interval when it comes back
            // on.
            last_pulse_ms_ = now_ms;
            if (seen_) {
                state_ = PanelFrameState::UNKNOWN;
            }
            return state_;
        }
        if (!seen_) {
            if (now_ms - first_set_up_ms_ >= kPanelFrameAbsentAfterMs) {
                state_ = PanelFrameState::ABSENT;
            }
            return state_;
        }
        if (now_ms - last_pulse_ms_ >= kPanelFrameLostAfterMs) {
            state_ = PanelFrameState::LOST;
        }
        return state_;
    }

    PanelFrameState state() const { return state_; }

private:
    bool started_ = false;
    bool seen_ = false;
    PanelFrameState state_ = PanelFrameState::UNKNOWN;
    uint32_t first_set_up_ms_ = 0;
    uint32_t last_pulse_ms_ = 0;
    uint32_t last_pulse_count_ = 0;
};

//------------------------------------------------------------------------------
// Switching off and on
//------------------------------------------------------------------------------

// The controller's power-down order is Display Off, Sleep In, then the reset
// pin, which may follow Sleep In no sooner than five frames (83 ms).
constexpr uint32_t kPanelSleepInSettleMs = 100U;

// A panel that has just been set up still holds the picture from before its
// reset, or noise after a supply dip. Its brightness stays at zero until the
// whole screen has been drawn again, or for this long if no drawing comes.
constexpr uint32_t kPanelRevealTimeoutMs = 250U;

// Decides when the picture of a freshly set-up panel is shown.
class PanelReveal {
public:
    // Call when the panel has been set up with its brightness at zero.
    void hide(uint32_t now_ms) {
        hidden_ = true;
        drawn_ = false;
        hidden_ms_ = now_ms;
    }

    // Call when a redraw of the screen has reached the panel.
    void screen_drawn() { drawn_ = true; }

    bool hidden() const { return hidden_; }

    // True once, when the brightness should be put back.
    bool due(uint32_t now_ms) {
        if (!hidden_ || (!drawn_ && now_ms - hidden_ms_ < kPanelRevealTimeoutMs)) {
            return false;
        }
        hidden_ = false;
        drawn_ = false;
        return true;
    }

private:
    bool hidden_ = false;
    bool drawn_ = false;
    uint32_t hidden_ms_ = 0;
};

//------------------------------------------------------------------------------
// Full reset while the display is on
//------------------------------------------------------------------------------

// What calls for a full reset while the display is on.
enum class PanelResetCause : uint8_t {
    FRAME_LOST,  // The frame signal stopped and a resend did not bring it back
    REQUESTED,   // Asked for from the web page
    COUNT,
};

constexpr const char* panel_reset_cause_name(PanelResetCause cause) {
    return cause == PanelResetCause::FRAME_LOST ? "frame signal lost" : "requested";
}

// The reset line also resets the touch controller, so a reset waits until the
// screen has been left alone this long...
constexpr uint32_t kPanelResetTouchQuietMs = 1500U;
// ...but no longer than this, so a contact that the touch controller never
// lets go of cannot hold a reset off for good.
constexpr uint32_t kPanelResetMaxWaitMs = 10000U;
// Closest spacing of two resets.
constexpr uint32_t kPanelResetMinSpacingMs = 2000U;
// A lost frame signal gets a resend first. Still lost this long after, or
// this long after the reset before, it gets a reset.
constexpr uint32_t kPanelFrameResetAfterMs = 2500U;
// Resets tried for one loss of the frame signal. Past that the panel is left
// alone until the display is next switched off or a reset is asked for, so a
// signal that cannot return never makes a healthy screen blink for good.
constexpr uint8_t kPanelFrameResetAttempts = 3;

// What following the frame signal asks of the caller.
struct PanelFrameEvents {
    bool lost = false;       // The signal has just stopped: resend the setup now
    bool recovered = false;  // The signal is back after having stopped
};

// Decides when a full reset runs.
class PanelResetSchedule {
public:
    // Asks for a reset. One reset serves every cause waiting for it, and is
    // put down to the first.
    void request(PanelResetCause cause, uint32_t now_ms) {
        if (pending_) {
            return;
        }
        pending_ = true;
        cause_ = cause;
        requested_ms_ = now_ms;
    }

    // Call every UI cycle with the state of the frame signal.
    PanelFrameEvents follow_frame(uint32_t now_ms, PanelFrameState state, bool panel_should_be_on) {
        PanelFrameEvents events;
        if (state == PanelFrameState::RUNNING) {
            events.recovered = frame_lost_;
            frame_lost_ = false;
            frame_reset_attempts_ = 0;
            return events;
        }
        if (state != PanelFrameState::LOST || !panel_should_be_on) {
            return events;
        }
        if (!frame_lost_) {
            frame_lost_ = true;
            frame_lost_ms_ = now_ms;
            events.lost = true;
            return events;
        }
        if (!pending_ && frame_reset_attempts_ < kPanelFrameResetAttempts &&
            now_ms - frame_lost_ms_ >= kPanelFrameResetAfterMs) {
            ++frame_reset_attempts_;
            frame_lost_ms_ = now_ms;
            request(PanelResetCause::FRAME_LOST, now_ms);
        }
        return events;
    }

    // True when a reset should run now. `touch_quiet` says the screen has
    // been left alone for kPanelResetTouchQuietMs.
    bool due(uint32_t now_ms, bool panel_should_be_on, bool touch_quiet) {
        if (!panel_should_be_on) {
            // A panel that is off on purpose was reset as it went off, and is
            // set up from scratch on waking.
            pending_ = false;
            return false;
        }
        if (!pending_) {
            return false;
        }
        if (ran_ && now_ms - last_finished_ms_ < kPanelResetMinSpacingMs) {
            return false;
        }
        return touch_quiet || now_ms - requested_ms_ >= kPanelResetMaxWaitMs;
    }

    // The cause of the reset that is about to run.
    PanelResetCause begin() {
        pending_ = false;
        return cause_;
    }

    // Call when the reset is done.
    void finished(uint32_t now_ms) {
        ran_ = true;
        last_finished_ms_ = now_ms;
    }

    bool pending() const { return pending_; }

private:
    bool pending_ = false;
    bool ran_ = false;
    bool frame_lost_ = false;
    PanelResetCause cause_ = PanelResetCause::FRAME_LOST;
    uint8_t frame_reset_attempts_ = 0;
    uint32_t requested_ms_ = 0;
    uint32_t last_finished_ms_ = 0;
    uint32_t frame_lost_ms_ = 0;
};
