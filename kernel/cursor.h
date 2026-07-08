// cursor.h — a software-drawn blinking text cursor, synced to the PIT
// timer tick (irq::g_tick_count) rather than its own separate timing
// mechanism. The blink rate is a direct function of the configured PIT
// frequency — see kernel.cpp for the actual Hz value.
//
// HOW IT WORKS: every so many timer ticks, we toggle the background
// color of whatever character cell the cursor is currently positioned
// over. The Terminal class needs to tell us where that is and needs to
// redraw that cell each time the blink state flips.
//
// IMPORTANT SUBTLETY: the cursor must NOT permanently alter the
// character/color that was actually typed there — it's just a visual
// overlay. Terminal always knows the "real" character+color at the
// cursor position, and the cursor draws over it temporarily, restoring
// the real appearance every other blink phase.

#pragma once
#include <stdint.h>

namespace cursor {

// One blink "phase" (visible, then invisible) lasts this many timer ticks.
// At a 100 Hz PIT (see kernel.cpp), 50 ticks = 500ms — a fairly standard,
// comfortable terminal cursor blink rate (most terminals use 400-600ms).
constexpr uint64_t BLINK_INTERVAL_TICKS = 50;

class BlinkState {
public:
    BlinkState() : last_tick_checked(0), visible(true) {}

    // Call this once per main-loop iteration (or any time you're about
    // to redraw the screen) with the current tick count. Returns true if
    // the visibility just changed (so the caller knows to actually redraw
    // the cursor cell) — avoids redundant redraws every single call.
    bool update(uint64_t current_tick) {
        if (current_tick - last_tick_checked >= BLINK_INTERVAL_TICKS) {
            last_tick_checked = current_tick;
            visible = !visible;
            return true;
        }
        return false;
    }

    bool is_visible() const { return visible; }

    // Forces the cursor to the "visible" state and resets timing — call
    // this whenever the user types something, so the cursor doesn't
    // appear to "blink off" right when they're actively typing (matches
    // the behavior of pretty much every real terminal/editor).
    void reset_to_visible(uint64_t current_tick) {
        visible = true;
        last_tick_checked = current_tick;
    }

private:
    uint64_t last_tick_checked;
    bool visible;
};

} // namespace cursor
