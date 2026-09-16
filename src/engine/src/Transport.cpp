#include "ruby/engine/Transport.h"

#include <algorithm>
#include <cmath>

namespace ruby::engine {

void Transport::setDuration(double seconds) noexcept {
    duration_ = std::max(0.0, seconds);
    time_ = std::clamp(time_, 0.0, duration_);
}

void Transport::setFrameRate(double fps) noexcept {
    fps_ = (fps > 0.0) ? fps : 30.0;
}

void Transport::setTime(double seconds) noexcept {
    time_ = std::clamp(seconds, 0.0, duration_);
}

void Transport::play() noexcept {
    if (duration_ <= 0.0) {
        return;
    }
    // Play at the very end restarts rather than no-oping.
    if (time_ >= duration_) {
        time_ = 0.0;
    }
    playing_ = true;
}

void Transport::stop() noexcept { playing_ = false; }

void Transport::toggle() noexcept {
    if (playing_) {
        stop();
    } else {
        play();
    }
}

int Transport::frame() const noexcept {
    return static_cast<int>(std::floor(time_ * fps_));
}

bool Transport::advance(double elapsedSeconds) noexcept {
    if (!playing_ || elapsedSeconds <= 0.0) {
        return false;
    }

    const int before = frame();
    time_ += elapsedSeconds;

    if (time_ >= duration_) {
        if (looping_ && duration_ > 0.0) {
            // Wrap by remainder, not to zero, so a long stall doesn't lose position.
            time_ = std::fmod(time_, duration_);
        } else {
            time_ = duration_;
            playing_ = false;
        }
    }
    return frame() != before;
}

}  // namespace ruby::engine
