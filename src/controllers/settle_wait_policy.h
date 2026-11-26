#pragma once

#include <cstdint>

#include "grind_events.h"

// A grind waits for the scale to read still before it acts on a weight: after
// taring, after the motor stops, before a correction pulse and before the
// final weight. On a bench that vibrates the scale never reads still, and
// those waits hold the grind until its timeout.
//
// A hand on the portafilter also keeps the scale from reading still, and
// unlike vibration it does not average out. From the readings alone the two
// cannot always be told apart. So every wait is for a still reading, as it
// always was, unless shaky-bench handling has been switched on for a grinder
// that stands on a vibrating bench. Then the waits have limits, and on a
// bench the grind itself finds to be shaking an average of the readings may
// stand in for a still one.

enum class SettleWait : uint8_t {
    WAITING,   // Not still yet, and nothing else may end the wait so far
    STILL,     // The scale reads still: use the settled reading
    UNSTEADY,  // The limit passed without a still reading
};

// What a wait does once its limit has passed without a still reading.
enum class PastLimit : uint8_t {
    CARRY_ON,      // The step needs no weight: go on whatever the scale does
    USE_AVERAGE,   // Act on an average, once the readings can be averaged
    KEEP_WAITING,  // Only a still reading will do
};

constexpr SettleWait settle_wait(bool scale_still, uint32_t waited_ms, uint32_t limit_ms) {
    return scale_still ? SettleWait::STILL
         : waited_ms >= limit_ms ? SettleWait::UNSTEADY
         : SettleWait::WAITING;
}

// Nothing moves between a pulse decision and the final weight, so the final
// weight continues the wait the decision already served instead of starting
// a second one.
constexpr bool continues_settle_wait(GrindPhase from, GrindPhase to) {
    return from == GrindPhase::PULSE_DECISION && to == GrindPhase::FINAL_SETTLING;
}

// Whether the grind has shown its bench to be shaking. Twice before the main
// run the vessel stands on the scale with the motor off, seconds apart: after
// taring and after the prime. On a quiet bench the scale comes to rest at
// least one of those times; a 3D printer on the same bench keeps it from
// doing so both times, while a hand rarely stays on through both.
//
// "Quiet" asks for more than one still half second, which a shaking scale
// produces by chance now and then: a whole still second after taring, or
// stillness soon after the prime's grounds have landed.
constexpr bool bench_is_shaking(bool quiet_after_tare, bool quiet_after_prime) {
    return !quiet_after_tare && !quiet_after_prime;
}

// Past its limit, a wait that needs no weight (after taring, after the motor
// stops) goes on only with shaky-bench handling.
constexpr PastLimit stillness_wait_past_limit(bool shaky_bench_handling) {
    return shaky_bench_handling ? PastLimit::CARRY_ON : PastLimit::KEEP_WAITING;
}

// Past its limit, a wait that acts on a weight may go by an average only with
// shaky-bench handling. A correction pulse adds coffee, so it also needs a
// bench this grind has found to be shaking. A time grind has no earlier waits
// to judge its bench by, and its final weight changes nothing but the number
// on the screen.
constexpr PastLimit weight_wait_past_limit(bool shaky_bench_handling, GrindMode mode, bool bench_shaking) {
    if (!shaky_bench_handling) {
        return PastLimit::KEEP_WAITING;
    }
    return (mode != GrindMode::WEIGHT || bench_shaking) ? PastLimit::USE_AVERAGE : PastLimit::KEEP_WAITING;
}
