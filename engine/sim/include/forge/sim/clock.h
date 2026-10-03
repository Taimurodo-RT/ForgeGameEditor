#pragma once

// Fixed-step clock: the game world advances in equal ticks (60 per second by
// default) whatever the frame rate, so a game behaves the same at 30, 60 or
// 240 FPS and replays stay exact. Frames add their real time; whole ticks
// come out. A frame that took too long runs at most max_ticks ticks and drops
// the rest (the game slows down instead of freezing while it catches up).

#include "forge/core/types.h"

namespace forge::sim {

class SimClock {
public:
    explicit SimClock(u32 ticks_per_second = 60, u32 max_ticks = 4)
        : step_(1.0 / static_cast<f64>(ticks_per_second)), max_ticks_(max_ticks) {}

    // Adds a frame's time; returns how many ticks to run now.
    u32 advance(f64 frame_seconds) {
        acc_ += frame_seconds > 0 ? frame_seconds : 0;
        u32 n = static_cast<u32>(acc_ / step_);
        if (n > max_ticks_) {
            n = max_ticks_;
            dropped_ += acc_ - static_cast<f64>(n) * step_;
            acc_ = static_cast<f64>(n) * step_;
        }
        acc_ -= static_cast<f64>(n) * step_;
        tick_ += n;
        return n;
    }

    u64 tick() const { return tick_; }                  // ticks run since the start
    f32 step() const { return static_cast<f32>(step_); } // seconds per tick
    // How far the frame is between the last tick and the next, 0..1: draw
    // moving things this much past their last tick for smooth motion on
    // monitors faster than the tick rate.
    f32 alpha() const { return static_cast<f32>(acc_ / step_); }
    f64 dropped_seconds() const { return dropped_; }

private:
    f64 step_;
    u32 max_ticks_;
    f64 acc_ = 0;
    f64 dropped_ = 0;
    u64 tick_ = 0;
};

} // namespace forge::sim
