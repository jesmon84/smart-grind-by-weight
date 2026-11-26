#pragma once

#include <cmath>
#include <cstdint>

// Knows the weight of the cup or portafilter standing on the scale, whatever
// the scale was last zeroed with.
//
// The removal guard needs that weight. A weight grind zeroes the scale with
// the vessel in place, so on the next grind the vessel reads about zero and
// the reading alone no longer tells. This tracker works on a level that
// taring does not move, and remembers the level the scale rests at when
// empty: the lowest level it has come to rest at. Whatever stands above that
// level is the vessel.
//
// The empty level follows the scale down at once but up only within
// `kEmptyBandG`. Sensor drift and crumbs are absorbed that way, while a
// vessel left standing on the scale never becomes the new "empty".
class VesselWeightTracker {
public:
    // A load lighter than this is not a vessel (the removal guard's minimum).
    static constexpr float kEmptyBandG = 5.0f;
    // The averaged level may wander this much and still count as at rest.
    static constexpr float kRestToleranceG = 1.0f;
    // Time inside that tolerance before the scale counts as at rest.
    static constexpr uint32_t kRestHoldMs = 1000U;
    // How it is fed: a level averaged over this window, this often. The
    // average keeps a vibrating bench inside the rest tolerance.
    static constexpr uint32_t kAverageWindowMs = 1000U;
    static constexpr uint32_t kObserveIntervalMs = 100U;

    // Forget everything. The next resting level becomes the empty level, so
    // a vessel already on the scale is unknown until it has been lifted off.
    void reset() {
        empty_valid_ = false;
        hold_valid_ = false;
    }

    // The scale is about to be disturbed (a grind starts). Rest has to be
    // established afresh afterwards; the empty level is kept.
    void interrupt() { hold_valid_ = false; }

    bool has_empty_level() const { return empty_valid_; }
    float empty_level_g() const { return empty_level_g_; }

    // Feed an averaged level in grams that taring does not move.
    void observe(float level_g, uint32_t now_ms) {
        if (!std::isfinite(level_g)) {
            hold_valid_ = false;
            return;
        }
        if (!hold_valid_ || std::fabs(level_g - hold_anchor_g_) > kRestToleranceG) {
            hold_anchor_g_ = level_g;
            hold_start_ms_ = now_ms;
            hold_valid_ = true;
            return;
        }
        if (now_ms - hold_start_ms_ < kRestHoldMs) {
            return;
        }

        // At rest. Lower than the known empty level, or close above it: this
        // is the empty scale. Further above it: something stands on the scale.
        if (!empty_valid_ || level_g < empty_level_g_ + kEmptyBandG) {
            empty_level_g_ = level_g;
            empty_valid_ = true;
        }
    }

    // Weight standing above the empty level; zero while that level is unknown.
    float vessel_weight_g(float level_g) const {
        if (!empty_valid_ || !std::isfinite(level_g)) {
            return 0.0f;
        }
        const float weight_g = level_g - empty_level_g_;
        return weight_g > 0.0f ? weight_g : 0.0f;
    }

private:
    float empty_level_g_ = 0.0f;
    bool empty_valid_ = false;
    float hold_anchor_g_ = 0.0f;
    uint32_t hold_start_ms_ = 0;
    bool hold_valid_ = false;
};

// The vessel's weight for the removal guard, from what is known as a grind
// starts.
//
// `reading_g` is what the scale reads. On a scale last zeroed empty that is
// the vessel's weight, and whenever it is large enough to be one it is used
// as it always was.
//
// A weight grind zeroes the scale with the vessel on, so on the next grind
// the vessel reads about nothing. `carried_g` is what stood on the scale when
// a grind last zeroed it, or zero when no grind has or something else has
// zeroed the scale since: reading plus carried is then the load over that
// zero.
//
// Only when that tells nothing either does `over_empty_scale_g`, the tracked
// weight, stand in. It comes last because one lighter rest of the scale (a
// tray taken off for cleaning) makes it too large until the next power-up.
inline float vessel_weight_estimate(float reading_g, float carried_g, float over_empty_scale_g) {
    if (reading_g >= VesselWeightTracker::kEmptyBandG) {
        return reading_g;
    }
    const float over_grind_zero_g = reading_g + carried_g;
    return over_grind_zero_g >= VesselWeightTracker::kEmptyBandG ? over_grind_zero_g : over_empty_scale_g;
}
