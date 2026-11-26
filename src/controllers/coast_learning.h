#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// How far short of the target a weight grind stops its motor.
//
// Coffee keeps landing after the motor stops: grounds still falling down the
// chute and what the burrs throw out as they run down. Measured as time, that
// is a property of the grinder: about a third of a second of flow on the
// grinder this was found on, grind after grind. Its weight is that time times
// the flow, and the flow changes with the beans and the grind setting, so the
// flow is taken from the grind in progress.
//
// The grinder used to work the time out afresh in every grind, from how long
// the first grounds took to show on the scale. With the motor running the
// readings swing too much for that: the same grinder came out anywhere from
// 40 to 460 ms, and a short one let the motor run too long, past the target,
// where no correction can take coffee out again. So the time is learnt from
// what really landed after each main run, and the motor stops a margin short
// of where that would end, leaving one small correction to finish the dose.

// Shortest and longest after-run time a grinder can plausibly have. A
// landing outside them is something else (a drop of retained grounds, a hand
// on the vessel) and teaches nothing.
constexpr float kCoastLearnMinMs = 100.0f;
constexpr float kCoastLearnMaxMs = 800.0f;
// Each grind moves the learnt time this part of the way to what it saw, and
// never by more than the step, so one odd grind cannot throw it off.
constexpr float kCoastLearnWeight = 0.5f;
constexpr float kCoastLearnMaxStepMs = 60.0f;
// The motor stops this much earlier than the learnt time says, so that the
// main run ends just under the target.
constexpr float kCoastLearnMarginG = 0.15f;
// Flows a grind's own reading is trusted within, for learning and stopping.
// A single bad scale reading can make the measured flow absurd for a moment.
constexpr float kCoastFlowMinGps = 0.5f;
constexpr float kCoastFlowMaxGps = 4.0f;
// However the numbers come out, the motor never stops further short than this.
constexpr float kCoastMaxStopOffsetG = 3.0f;

// The after-run time `landed_g` amounts to at `flow_gps`, if both make sense.
inline bool coast_ms_from_landing(float landed_g, float flow_gps, float* coast_ms_out) {
    if (!std::isfinite(landed_g) || !std::isfinite(flow_gps) ||
        flow_gps < kCoastFlowMinGps || flow_gps > kCoastFlowMaxGps) {
        return false;
    }
    const float coast_ms = landed_g / flow_gps * 1000.0f;
    if (coast_ms < kCoastLearnMinMs || coast_ms > kCoastLearnMaxMs) {
        return false;
    }
    if (coast_ms_out) *coast_ms_out = coast_ms;
    return true;
}

class CoastLearner {
public:
    // From storage. Values that could not have been learnt start afresh.
    void restore(float coast_ms, uint32_t grinds) {
        if (grinds == 0 || !std::isfinite(coast_ms) || coast_ms < kCoastLearnMinMs || coast_ms > kCoastLearnMaxMs) {
            reset();
            return;
        }
        coast_ms_ = coast_ms;
        grinds_ = grinds;
    }

    void reset() {
        coast_ms_ = 0.0f;
        grinds_ = 0;
    }

    // Known after one grind: even a single measurement is far closer than the
    // estimate it replaces.
    bool known() const { return grinds_ > 0; }
    float coast_ms() const { return coast_ms_; }
    uint32_t grinds() const { return grinds_; }

    // Takes in one grind's after-run time. False when it is not plausible.
    bool learn(float observed_ms) {
        if (!std::isfinite(observed_ms) || observed_ms < kCoastLearnMinMs || observed_ms > kCoastLearnMaxMs) {
            return false;
        }
        if (!known()) {
            coast_ms_ = observed_ms;
        } else {
            const float step = std::clamp((observed_ms - coast_ms_) * kCoastLearnWeight,
                                          -kCoastLearnMaxStepMs, kCoastLearnMaxStepMs);
            coast_ms_ += step;
        }
        if (grinds_ < UINT32_MAX) ++grinds_;
        return true;
    }

    // How far short of the target the motor stops while `flow_gps` runs. A
    // flow that is no number at all counts as the fastest trusted one: that
    // stops the motor early, and a correction makes up the rest.
    float stop_offset_g(float flow_gps) const {
        const float flow = std::isfinite(flow_gps) ? std::clamp(flow_gps, kCoastFlowMinGps, kCoastFlowMaxGps)
                                                   : kCoastFlowMaxGps;
        return std::min(coast_ms_ / 1000.0f * flow + kCoastLearnMarginG, kCoastMaxStopOffsetG);
    }

private:
    float coast_ms_ = 0.0f;
    uint32_t grinds_ = 0;
};
