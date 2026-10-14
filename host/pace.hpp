#pragma once

// The page's pacing decisions that need no browser, so they can be tested without one. host/web.cpp does
// the waiting itself; this says how long a wait may last and when one has to be given up.

#include <cstdint>
#include <limits>

namespace ps2web {

// A wait with no time limit.
inline constexpr double kUntilShown = std::numeric_limits<double>::infinity();

// A present waits until the display has shown the frame before it, or a busy main thread starves the
// browser's own drawing and most frames never reach the screen. On a display that refreshes slower than
// the game runs, that wait alone would slow the game down to the display. So the wait ends once a whole
// frame of game time has passed since the last present: the frame is presented anyway and the one before
// it is never seen. Two rules keep the protection:
//   - a main thread that is behind (it arrives a frame late and has not emptied the render queue since
//     the last present) waits for the display however long it takes, as it always did;
//   - no two presents in a row go ahead without the display, so at least every second frame is shown.
// A display at or above the game's rate shows each frame within one game frame, so nothing changes there.
struct PresentGate {
    static constexpr int kMaxUnshown = 1;

    double presented_at = 0.0;   // when the last frame was presented, in ms
    int unshown = 0;             // presents in a row made before the display showed the frame before them
    bool caught_up = false;      // the main thread has found the render queue empty since the last present

    // How long the next present may wait for the display to show the last frame, in ms: kUntilShown, or
    // a time after which it goes ahead without the display (0: at once).
    double wait_limit(double now, double period) const;
    void queue_empty() { caught_up = true; }
    // A frame has been presented; `last_shown` says whether the display had shown the one before it.
    void presented(double now, bool last_shown);
};

// The emulation thread sleeps while the audio queue is over its target and trusts the audio device to
// drain it. If the device stops taking samples the queue never drains and the game would wait for ever.
// One look a millisecond at the queue's read position: once it has not moved for kLooks looks in a row
// the device counts as stopped, and the host paces by the clock until it takes samples again.
struct AudioStall {
    static constexpr int kLooks = 200;

    std::uint32_t seen = 0;
    int still = 0;

    explicit AudioStall(std::uint32_t read) : seen(read) {}
    bool stopped(std::uint32_t read);
};

}
