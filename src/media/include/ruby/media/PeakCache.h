#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ruby/media/AudioDecoder.h"

namespace ruby::media {

// Mipmap of waveform peaks, precomputed since a fixed bucket rate can't serve both
// full-zoom-out and near-sample zoom-in, and decoding on demand can't keep up with
// scrubbing.

// 150/sec: not tied to any one composition's frame rate (media is pooled across
// compositions at different fps), and finer than any rate anyone edits at.
inline constexpr double kBasePeaksPerSecond = 150.0;

// Each level is 1/4 the resolution of the one before: reaches an hour of audio in a
// handful of levels without ever overshooting by more than 4x.
inline constexpr int kPeakLevelRatio = 4;

struct PeakLevel {
    double bucketsPerSecond = 0.0;
    std::vector<float> low;
    std::vector<float> high;

    [[nodiscard]] bool empty() const noexcept { return low.empty(); }
    [[nodiscard]] std::size_t count() const noexcept { return low.size(); }
};

class PeakPyramid {
public:
    // Builds the base level from samples, then each level above from the one below
    // (exact for min/max, unlike RMS — keep that in mind before switching metrics).
    static PeakPyramid build(const AudioBuffer& buffer);

    // Coarsest level with >= 1 bucket per pixel. Never null for a non-empty pyramid;
    // falls back to the finest level when over-zoomed.
    [[nodiscard]] const PeakLevel* levelFor(double secondsPerPixel) const;

    [[nodiscard]] const std::vector<PeakLevel>& levels() const noexcept { return levels_; }
    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
    [[nodiscard]] double duration() const noexcept { return duration_; }

    // Best-effort: a failed load is a cache miss, not an error, since the source can
    // always be redecoded.
    bool save(const std::string& file) const;
    bool load(const std::string& file);

private:
    std::vector<PeakLevel> levels_;
    double duration_ = 0.0;
};

// Cache path for a file's peaks, keyed on path+size+mtime so a re-exported clip that
// kept its name doesn't get served stale peaks.
[[nodiscard]] std::string peakCachePath(const std::string& directory,
                                        const std::string& mediaPath);

// Whether a clip's peaks are ready. Distinct "not ready yet" state so callers don't
// have to guess whether an empty waveform means silence or means wait.
enum class ConformState {
    Absent,   // never asked for
    Working,  // decoding now
    Ready,    // peaks available
    Failed,   // no audio, or unreadable
};

}  // namespace ruby::media
