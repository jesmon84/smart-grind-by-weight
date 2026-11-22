#pragma once

#include <cstdint>

// Swiping from a profile to the Wi-Fi or menu page passes over the profiles in
// between, and each one comes into view on the way. A profile is only stored
// as the one to return to after a restart once it has stayed in view this
// long (or a grind was started with it).
constexpr uint32_t kProfileSettleMs = 2000U;

class ProfileSettleTracker {
public:
    // A profile page came into view.
    void profile_shown(uint32_t now_ms) {
        pending_ = true;
        shown_at_ms_ = now_ms;
    }

    // The view left the profile pages before the profile settled.
    void cancel() { pending_ = false; }

    // True while a profile in view has not yet been stored.
    bool pending() const { return pending_; }

    // True once, when the profile in view has stayed long enough to store.
    bool settled(uint32_t now_ms) {
        if (!pending_ || now_ms - shown_at_ms_ < kProfileSettleMs) {
            return false;
        }
        pending_ = false;
        return true;
    }

private:
    bool pending_ = false;
    uint32_t shown_at_ms_ = 0;
};
