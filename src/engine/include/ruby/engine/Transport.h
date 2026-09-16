#pragma once

namespace ruby::engine {

// Playback state. No internal timer — advances by actual wall-clock elapsed time, so a
// slow decoder drops frames instead of playing back at the wrong speed.
class Transport {
public:
    void setDuration(double seconds) noexcept;
    void setFrameRate(double fps) noexcept;

    [[nodiscard]] double duration() const noexcept { return duration_; }
    [[nodiscard]] double frameRate() const noexcept { return fps_; }

    [[nodiscard]] double time() const noexcept { return time_; }
    void setTime(double seconds) noexcept;

    [[nodiscard]] bool playing() const noexcept { return playing_; }
    void play() noexcept;
    void stop() noexcept;
    void toggle() noexcept;

    [[nodiscard]] bool looping() const noexcept { return looping_; }
    void setLooping(bool loop) noexcept { looping_ = loop; }

    // Advances by real elapsed seconds. Returns true if the displayed frame changed
    // (i.e. a repaint is worth doing).
    bool advance(double elapsedSeconds) noexcept;

    [[nodiscard]] int frame() const noexcept;

private:
    double duration_ = 12.0;
    double fps_ = 30.0;
    double time_ = 0.0;
    bool playing_ = false;
    bool looping_ = true;
};

}  // namespace ruby::engine
