#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ruby::media {

// Decoded audio, interleaved float, whole track buffered rather than streamed —
// cheap (~90MB for 4min stereo/48kHz) and every consumer needs random access anyway.
struct AudioBuffer {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;  // interleaved, [-1, 1]

    [[nodiscard]] std::size_t frameCount() const noexcept {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
    [[nodiscard]] double duration() const noexcept {
        return sampleRate > 0 ? static_cast<double>(frameCount()) / sampleRate : 0.0;
    }
    [[nodiscard]] bool valid() const noexcept {
        return sampleRate > 0 && channels > 0 && !samples.empty();
    }

    // Mono sum, for analysis and drawing; neither cares about stereo.
    [[nodiscard]] float monoAt(std::size_t frame) const noexcept;
};

// Min/max per time bucket, computed once so the timeline isn't rescanning raw samples
// on every repaint.
struct WaveformPeaks {
    double bucketsPerSecond = 0.0;
    std::vector<float> low;
    std::vector<float> high;

    [[nodiscard]] bool empty() const noexcept { return low.empty(); }
};

class AudioDecoder {
public:
    // Resamples to `targetRate` and keeps the source channel count (capped at stereo).
    // Returns nullopt when the file has no audio or cannot be read.
    [[nodiscard]] static std::optional<AudioBuffer> decode(const std::string& path,
                                                           int targetRate = 48000);

    [[nodiscard]] static WaveformPeaks peaks(const AudioBuffer& buffer,
                                             double bucketsPerSecond);
};

}  // namespace ruby::media
