// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include "controllers/grind_session_result.h"

#include <cassert>

int main() {
    assert(!is_completed_grind_result(GrindSessionResult::UNKNOWN));
    assert(is_completed_grind_result(GrindSessionResult::SUCCESS));
    assert(is_completed_grind_result(GrindSessionResult::OVERSHOOT));
    assert(is_completed_grind_result(GrindSessionResult::MAX_PULSES));
    assert(!is_completed_grind_result(GrindSessionResult::TIMEOUT));
    assert(!is_completed_grind_result(GrindSessionResult::ERROR));
    return 0;
}
