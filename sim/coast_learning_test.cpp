// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>

#include "controllers/coast_learning.h"

namespace {

bool near(float value, float expected, float tolerance) { return std::fabs(value - expected) <= tolerance; }

void the_grinders_own_grinds_give_its_after_run_time() {
    // Read from a real grinder: what landed after the cut, at the flow then.
    float ms = 0.0f;
    assert(coast_ms_from_landing(0.65f, 2.19f, &ms) && near(ms, 297.0f, 1.0f));
    assert(coast_ms_from_landing(0.77f, 2.12f, &ms) && near(ms, 363.0f, 1.0f));
    assert(coast_ms_from_landing(0.76f, 2.21f, &ms) && near(ms, 344.0f, 1.0f));
}

void implausible_landings_teach_nothing() {
    float ms = -1.0f;
    assert(!coast_ms_from_landing(0.05f, 2.0f, &ms));   // 25 ms: nothing landed to speak of
    assert(!coast_ms_from_landing(2.0f, 2.0f, &ms));    // 1 s: a drop of retained grounds
    assert(!coast_ms_from_landing(-0.4f, 2.0f, &ms));   // A hand lifting the vessel
    assert(!coast_ms_from_landing(0.7f, 0.2f, &ms));    // Flow too slow to trust
    assert(!coast_ms_from_landing(0.7f, 40.0f, &ms));   // A bad reading's flow
    assert(!coast_ms_from_landing(NAN, 2.0f, &ms));
    assert(!coast_ms_from_landing(0.7f, INFINITY, &ms));
    assert(ms == -1.0f);  // Left alone

    CoastLearner learner;
    assert(!learner.learn(50.0f));
    assert(!learner.learn(900.0f));
    assert(!learner.learn(NAN));
    assert(!learner.known());
}

void one_grind_is_enough_to_use_it() {
    CoastLearner learner;
    assert(!learner.known());
    assert(learner.learn(340.0f));
    assert(learner.known());
    assert(learner.grinds() == 1);
    assert(near(learner.coast_ms(), 340.0f, 0.01f));
}

void the_flow_of_the_grind_in_progress_sets_the_stop() {
    // New beans that flow faster stop earlier at once, with nothing relearnt.
    CoastLearner learner;
    learner.learn(340.0f);
    assert(near(learner.stop_offset_g(2.2f), 0.34f * 2.2f + kCoastLearnMarginG, 0.001f));
    assert(near(learner.stop_offset_g(1.2f), 0.34f * 1.2f + kCoastLearnMarginG, 0.001f));
    assert(learner.stop_offset_g(3.0f) > learner.stop_offset_g(2.0f));
}

void an_absurd_flow_cannot_push_the_stop_past_the_target() {
    CoastLearner learner;
    learner.learn(340.0f);
    // A bad reading can show next to no flow: the stop still keeps the margin
    // and the slowest trusted flow's share.
    assert(learner.stop_offset_g(0.0f) >= kCoastLearnMarginG + 0.34f * kCoastFlowMinGps - 0.001f);
    // No number at all stops the motor early, never not at all.
    assert(std::isfinite(learner.stop_offset_g(NAN)));
    assert(learner.stop_offset_g(NAN) >= learner.stop_offset_g(2.0f));
    // Or an enormous one: the stop is never further short than the limit.
    assert(learner.stop_offset_g(500.0f) <= kCoastMaxStopOffsetG);
    learner.restore(kCoastLearnMaxMs, 5);
    assert(learner.stop_offset_g(kCoastFlowMaxGps) <= kCoastMaxStopOffsetG);
}

void one_odd_grind_moves_it_only_a_little() {
    CoastLearner learner;
    learner.learn(340.0f);
    learner.learn(790.0f);  // Plausible but far off
    assert(near(learner.coast_ms(), 340.0f + kCoastLearnMaxStepMs, 0.01f));
    // A small difference is taken half way.
    CoastLearner other;
    other.learn(340.0f);
    other.learn(360.0f);
    assert(near(other.coast_ms(), 350.0f, 0.01f));
}

void a_real_change_is_followed_within_a_few_grinds() {
    CoastLearner learner;
    learner.learn(300.0f);
    int grinds = 0;
    while (std::fabs(learner.coast_ms() - 450.0f) > 20.0f) {
        learner.learn(450.0f);
        ++grinds;
        assert(grinds < 6);
    }
}

void storage_round_trips_and_rejects_nonsense() {
    CoastLearner learner;
    learner.restore(345.0f, 12);
    assert(learner.known() && near(learner.coast_ms(), 345.0f, 0.01f) && learner.grinds() == 12);
    learner.restore(345.0f, 0);
    assert(!learner.known());
    learner.restore(5000.0f, 3);
    assert(!learner.known());
    learner.restore(NAN, 3);
    assert(!learner.known());
}

}  // namespace

int main() {
    the_grinders_own_grinds_give_its_after_run_time();
    implausible_landings_teach_nothing();
    one_grind_is_enough_to_use_it();
    the_flow_of_the_grind_in_progress_sets_the_stop();
    an_absurd_flow_cannot_push_the_stop_past_the_target();
    one_odd_grind_moves_it_only_a_little();
    a_real_change_is_followed_within_a_few_grinds();
    storage_round_trips_and_rejects_nonsense();
    return 0;
}
