#include "pace.hpp"

namespace ps2web {

double PresentGate::wait_limit(double now, double period) const {
    if (unshown >= kMaxUnshown) return kUntilShown;
    const double left = presented_at + period - now;
    if (left > 0.0) return left;   // early: this frame's work took less than a game frame, so the main thread is not what is slow
    return caught_up ? 0.0 : kUntilShown;
}

void PresentGate::presented(double now, bool last_shown) {
    unshown = last_shown ? 0 : unshown + 1;
    presented_at = now;
    caught_up = false;
}

bool AudioStall::stopped(std::uint32_t read) {
    if (read != seen) {
        seen = read;
        still = 0;
        return false;
    }
    return ++still >= kLooks;
}

}
