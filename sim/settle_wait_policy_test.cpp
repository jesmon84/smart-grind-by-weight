// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>

#include "controllers/grind_timeout_text.h"
#include "controllers/settle_wait_policy.h"

namespace {

void a_still_scale_ends_the_wait_at_once() {
    assert(settle_wait(true, 0, 2500) == SettleWait::STILL);
    assert(settle_wait(true, 2499, 2500) == SettleWait::STILL);
    // Still wins over the limit: the settled reading is the better one.
    assert(settle_wait(true, 60000, 2500) == SettleWait::STILL);
}

void an_unsteady_scale_waits_only_up_to_the_limit() {
    assert(settle_wait(false, 0, 2500) == SettleWait::WAITING);
    assert(settle_wait(false, 2499, 2500) == SettleWait::WAITING);
    assert(settle_wait(false, 2500, 2500) == SettleWait::UNSTEADY);
    assert(settle_wait(false, 30000, 2500) == SettleWait::UNSTEADY);
    // A limit of zero means no waiting at all.
    assert(settle_wait(false, 0, 0) == SettleWait::UNSTEADY);
}

void only_the_final_weight_continues_a_wait() {
    assert(continues_settle_wait(GrindPhase::PULSE_DECISION, GrindPhase::FINAL_SETTLING));
    // Grounds are still landing after the motor stops or a time grind ends.
    assert(!continues_settle_wait(GrindPhase::PULSE_SETTLING, GrindPhase::FINAL_SETTLING));
    assert(!continues_settle_wait(GrindPhase::TIME_GRINDING, GrindPhase::FINAL_SETTLING));
    assert(!continues_settle_wait(GrindPhase::PULSE_SETTLING, GrindPhase::PULSE_DECISION));
    assert(!continues_settle_wait(GrindPhase::PULSE_DECISION, GrindPhase::PULSE_EXECUTE));
}

void a_bench_shakes_only_if_the_scale_never_came_to_rest() {
    // Quiet after taring, quiet after the prime.
    assert(bench_is_shaking(false, false));
    // Coming to rest once is enough to call the bench quiet: a hand that was
    // on the vessel during one of the two waits does not make it a shaking one.
    assert(!bench_is_shaking(true, false));
    assert(!bench_is_shaking(false, true));
    assert(!bench_is_shaking(true, true));
}

void without_shaky_bench_handling_every_wait_is_for_a_still_reading() {
    assert(stillness_wait_past_limit(false) == PastLimit::KEEP_WAITING);
    const GrindMode modes[] = {GrindMode::WEIGHT, GrindMode::TIME, GrindMode::MANUAL};
    for (GrindMode mode : modes) {
        assert(weight_wait_past_limit(false, mode, true) == PastLimit::KEEP_WAITING);
        assert(weight_wait_past_limit(false, mode, false) == PastLimit::KEEP_WAITING);
    }
}

void with_it_only_a_shaking_bench_lets_a_pulse_go_by_an_average() {
    assert(stillness_wait_past_limit(true) == PastLimit::CARRY_ON);
    assert(weight_wait_past_limit(true, GrindMode::WEIGHT, true) == PastLimit::USE_AVERAGE);
    assert(weight_wait_past_limit(true, GrindMode::WEIGHT, false) == PastLimit::KEEP_WAITING);
    // A time grind only reports its final weight; it has no earlier waits to
    // judge the bench by.
    assert(weight_wait_past_limit(true, GrindMode::TIME, false) == PastLimit::USE_AVERAGE);
    assert(weight_wait_past_limit(true, GrindMode::MANUAL, false) == PastLimit::USE_AVERAGE);
}

void timeout_words_say_what_happened() {
    const GrindPhase after_taring[] = {
        GrindPhase::PRIME,          GrindPhase::PRIME_SETTLING, GrindPhase::PREDICTIVE,
        GrindPhase::PULSE_SETTLING, GrindPhase::PULSE_DECISION, GrindPhase::PULSE_EXECUTE,
        GrindPhase::FINAL_SETTLING,
    };
    for (GrindPhase phase : after_taring) {
        const GrindTimeoutText text = grind_timeout_text(GrindMode::WEIGHT, phase);
        assert(std::strcmp(text.title, "TIMEOUT") == 0);
        assert(std::strcmp(text.message, "Target not reached") == 0);
    }

    const GrindPhase before_the_motor[] = {
        GrindPhase::INITIALIZING, GrindPhase::SETUP, GrindPhase::TARING, GrindPhase::TARE_CONFIRM,
    };
    for (GrindPhase phase : before_the_motor) {
        assert(std::strcmp(grind_timeout_text(GrindMode::WEIGHT, phase).message, "Could not tare") == 0);
    }

    assert(std::strcmp(grind_timeout_text(GrindMode::TIME, GrindPhase::TIME_GRINDING).message,
                       "Time limit reached") == 0);

    // The manual safety cut-off keeps its own words and the default heading.
    const GrindTimeoutText manual = grind_timeout_text(GrindMode::MANUAL, GrindPhase::MANUAL_GRINDING);
    assert(manual.title == nullptr);
    assert(std::strcmp(manual.message, "30s safety limit") == 0);
}

void words_fit_the_error_screen() {
    // GrindEventData holds a 32-character message and a 16-character title.
    const GrindMode modes[] = {GrindMode::WEIGHT, GrindMode::TIME, GrindMode::MANUAL};
    for (GrindMode mode : modes) {
        for (int phase = 0; phase <= static_cast<int>(GrindPhase::PURGE_CONFIRM); ++phase) {
            const GrindTimeoutText text = grind_timeout_text(mode, static_cast<GrindPhase>(phase));
            assert(text.message != nullptr && std::strlen(text.message) < sizeof(GrindEventData::error_message));
            assert(text.title == nullptr || std::strlen(text.title) < sizeof(GrindEventData::error_title));
        }
    }
}

}  // namespace

int main() {
    a_still_scale_ends_the_wait_at_once();
    an_unsteady_scale_waits_only_up_to_the_limit();
    only_the_final_weight_continues_a_wait();
    a_bench_shakes_only_if_the_scale_never_came_to_rest();
    without_shaky_bench_handling_every_wait_is_for_a_still_reading();
    with_it_only_a_shaking_bench_lets_a_pulse_go_by_an_average();
    timeout_words_say_what_happened();
    words_fit_the_error_screen();
    return 0;
}
