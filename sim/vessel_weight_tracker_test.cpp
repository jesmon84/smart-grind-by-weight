// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>

#include "controllers/net_weight_guard.h"
#include "controllers/vessel_weight_tracker.h"

namespace {

constexpr uint32_t kStepMs = 100;
// The level taring does not move sits far from zero and may be negative.
constexpr float kEmpty = -1132.4f;
constexpr float kPortafilter = 410.0f;

struct Scale {
    VesselWeightTracker tracker;
    uint32_t now_ms = 0;

    void hold(float level_g, uint32_t duration_ms) {
        for (uint32_t t = 0; t < duration_ms; t += kStepMs) {
            now_ms += kStepMs;
            tracker.observe(level_g, now_ms);
        }
    }

    float vessel(float level_g) const { return tracker.vessel_weight_g(level_g); }
};

bool near(float value, float expected, float tolerance = 0.05f) {
    return std::fabs(value - expected) <= tolerance;
}

void vessel_is_unknown_before_the_scale_has_rested() {
    Scale scale;
    assert(!scale.tracker.has_empty_level());
    assert(scale.vessel(kEmpty + kPortafilter) == 0.0f);
    scale.hold(kEmpty, 900);
    assert(!scale.tracker.has_empty_level());
    scale.hold(kEmpty, 300);
    assert(scale.tracker.has_empty_level());
}

void first_grind_after_power_up() {
    Scale scale;
    scale.hold(kEmpty, 3000);
    assert(near(scale.vessel(kEmpty + kPortafilter), kPortafilter));
    // Resting with the portafilter on does not make it the new empty level.
    scale.hold(kEmpty + kPortafilter, 600000);
    assert(near(scale.vessel(kEmpty + kPortafilter), kPortafilter));
}

void later_grinds_know_the_vessel_too() {
    // The case that used to lose the reference: every grind tares with the
    // portafilter on, so its reading before the next tare is about zero.
    Scale scale;
    scale.hold(kEmpty, 3000);
    scale.hold(kEmpty + kPortafilter, 2000);          // placed, grind runs
    scale.tracker.interrupt();
    scale.hold(kEmpty + kPortafilter + 18.0f, 4000);  // result shown, grounds in
    scale.hold(kEmpty, 20000);                        // taken to the machine
    scale.hold(kEmpty + kPortafilter + 0.5f, 2000);   // back, a few grounds left in
    assert(near(scale.vessel(kEmpty + kPortafilter + 0.5f), kPortafilter + 0.5f));
}

void second_dose_without_lifting_counts_the_grounds() {
    Scale scale;
    scale.hold(kEmpty, 3000);
    scale.hold(kEmpty + kPortafilter + 18.0f, 5000);
    assert(near(scale.vessel(kEmpty + kPortafilter + 18.0f), kPortafilter + 18.0f));
}

void power_up_with_the_vessel_on() {
    Scale scale;
    scale.hold(kEmpty + kPortafilter, 5000);
    // Nothing to compare with yet: the guard stays off, as it always did.
    assert(scale.vessel(kEmpty + kPortafilter) == 0.0f);
    scale.hold(kEmpty, 1500);  // lifted off once
    assert(near(scale.vessel(kEmpty + kPortafilter), kPortafilter));
}

void drift_and_crumbs_move_the_empty_level() {
    Scale scale;
    scale.hold(kEmpty, 3000);
    // 2 g of warm-up drift, in steps the rest detector sees as separate.
    for (int step = 1; step <= 4; ++step) {
        scale.hold(kEmpty + 0.5f * step, 3000);
    }
    assert(near(scale.tracker.empty_level_g(), kEmpty + 2.0f));
    scale.hold(kEmpty + 2.0f + 3.0f, 3000);  // spilled grounds
    assert(near(scale.tracker.empty_level_g(), kEmpty + 5.0f));
    assert(near(scale.vessel(kEmpty + 5.0f + kPortafilter), kPortafilter));
    // Downward drift is followed at once.
    scale.hold(kEmpty - 1.5f, 3000);
    assert(near(scale.tracker.empty_level_g(), kEmpty - 1.5f));
}

void a_light_cup_is_a_vessel_not_drift() {
    Scale scale;
    scale.hold(kEmpty, 3000);
    scale.hold(kEmpty + 60.0f, 3600000);  // left standing for an hour
    assert(near(scale.tracker.empty_level_g(), kEmpty));
    assert(near(scale.vessel(kEmpty + 60.0f), 60.0f));
}

void swapping_vessels_without_an_empty_rest() {
    Scale scale;
    scale.hold(kEmpty, 3000);
    scale.hold(kEmpty + kPortafilter, 3000);
    scale.hold(kEmpty + 60.0f, 3000);  // portafilter off, cup on within a moment
    assert(near(scale.vessel(kEmpty + 60.0f), 60.0f));
}

void vibration_does_not_block_rest() {
    Scale scale;
    const float jitter[] = {0.3f, -0.2f, 0.1f, -0.3f, 0.2f, 0.0f};
    for (int i = 0; i < 30; ++i) {
        scale.now_ms += kStepMs;
        scale.tracker.observe(kEmpty + jitter[i % 6], scale.now_ms);
    }
    assert(scale.tracker.has_empty_level());
    assert(near(scale.tracker.empty_level_g(), kEmpty, 0.4f));
}

void a_moving_scale_teaches_nothing() {
    Scale scale;
    scale.hold(kEmpty, 3000);
    // Portafilter being lifted and wobbled about below the empty level for
    // twenty seconds: never at rest.
    const float wobble[] = {-40.0f, -25.0f, -60.0f, -10.0f, -35.0f};
    for (int i = 0; i < 200; ++i) {
        scale.now_ms += kStepMs;
        scale.tracker.observe(kEmpty + wobble[i % 5], scale.now_ms);
    }
    assert(near(scale.tracker.empty_level_g(), kEmpty));
}

void a_grind_interrupts_the_rest() {
    Scale scale;
    scale.hold(kEmpty + 3.0f, 900);
    scale.tracker.interrupt();
    // One reading at the old level is not a second of rest.
    scale.hold(kEmpty + 3.0f, 100);
    assert(!scale.tracker.has_empty_level());
    scale.hold(kEmpty + 3.0f, 1100);
    assert(scale.tracker.has_empty_level());
}

void invalid_readings_and_reset() {
    Scale scale;
    scale.hold(kEmpty, 3000);
    scale.tracker.observe(NAN, scale.now_ms += kStepMs);
    scale.tracker.observe(INFINITY, scale.now_ms += kStepMs);
    assert(near(scale.tracker.empty_level_g(), kEmpty));
    assert(scale.vessel(NAN) == 0.0f);
    assert(scale.vessel(kEmpty - 50.0f) == 0.0f);  // below empty is not a vessel

    scale.tracker.reset();
    assert(!scale.tracker.has_empty_level());
    assert(scale.vessel(kEmpty + kPortafilter) == 0.0f);
}

void clock_wrap_is_harmless() {
    Scale scale;
    scale.now_ms = UINT32_MAX - 500;
    scale.hold(kEmpty, 2000);
    assert(scale.tracker.has_empty_level());
}

void removal_is_caught_on_a_scale_zeroed_with_the_vessel() {
    Scale scale;
    scale.hold(kEmpty, 3000);
    scale.hold(kEmpty + kPortafilter, 2000);

    // The scale was last zeroed with the portafilter on, so it reads 0.5 g.
    const float reading_before_tare = 0.5f;
    NetWeightRemovalGuard old_guard;
    old_guard.reset(reading_before_tare);
    assert(!old_guard.has_reference());

    NetWeightRemovalGuard guard;
    guard.reset(scale.vessel(kEmpty + kPortafilter));
    assert(guard.has_reference());
    // Pushed up hard in its holder: still there.
    assert(!guard.update(-300.0f));
    assert(!guard.update(-300.0f));
    assert(!guard.update(-300.0f));
    // Lifted off with 12 g of grounds in it.
    assert(!guard.update(-422.0f));
    assert(!guard.update(-422.0f));
    assert(guard.update(-422.0f));
}

void a_reading_that_can_be_a_vessel_is_used_as_it_always_was() {
    // First grind after power-up: the reading tells.
    assert(near(vessel_weight_estimate(410.0f, 0.0f, 410.0f), 410.0f));
    // Whatever else is known, a reading of vessel size stands: the tracker
    // may be fooled by a tray taken off, the carried weight by a portafilter
    // that was off the scale while the last grind took its zero.
    assert(near(vessel_weight_estimate(410.0f, 0.0f, 560.0f), 410.0f));
    assert(near(vessel_weight_estimate(410.0f, 410.0f, 410.0f), 410.0f));
    assert(near(vessel_weight_estimate(18.0f, 410.0f, 428.0f), 18.0f));
    assert(near(vessel_weight_estimate(5.0f, 300.0f, 900.0f), 5.0f));
}

void the_weight_at_a_grinds_zero_fills_in() {
    // Zeroed with the portafilter on by the last grind: reads next to nothing.
    assert(near(vessel_weight_estimate(0.4f, 410.0f, 410.4f), 410.4f));
    assert(near(vessel_weight_estimate(0.4f, 410.0f, 560.4f), 410.4f));
    assert(near(vessel_weight_estimate(-0.7f, 410.0f, 0.0f), 409.3f));
    // A lighter cup after the portafilter reads far below that zero.
    assert(near(vessel_weight_estimate(-350.0f, 410.0f, 60.0f), 60.0f));
}

void the_tracker_stands_in_when_no_zero_tells() {
    // Powered up with the portafilter on, lifted off once since.
    assert(near(vessel_weight_estimate(0.4f, 0.0f, 410.4f), 410.4f));
    // Nothing known at all: no vessel, and so no guard.
    assert(vessel_weight_estimate(0.4f, 0.0f, 0.0f) < VesselWeightTracker::kEmptyBandG);
    // Below the smallest vessel the sum tells nothing either.
    assert(near(vessel_weight_estimate(-2.0f, 4.0f, 55.0f), 55.0f));
}

}  // namespace

int main() {
    vessel_is_unknown_before_the_scale_has_rested();
    first_grind_after_power_up();
    later_grinds_know_the_vessel_too();
    second_dose_without_lifting_counts_the_grounds();
    power_up_with_the_vessel_on();
    drift_and_crumbs_move_the_empty_level();
    a_light_cup_is_a_vessel_not_drift();
    swapping_vessels_without_an_empty_rest();
    vibration_does_not_block_rest();
    a_moving_scale_teaches_nothing();
    a_grind_interrupts_the_rest();
    invalid_readings_and_reset();
    clock_wrap_is_harmless();
    removal_is_caught_on_a_scale_zeroed_with_the_vessel();
    a_reading_that_can_be_a_vessel_is_used_as_it_always_was();
    the_weight_at_a_grinds_zero_fills_in();
    the_tracker_stands_in_when_no_zero_tells();
    return 0;
}
