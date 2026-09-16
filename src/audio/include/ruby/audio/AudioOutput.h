#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "ruby/media/AudioDecoder.h"

namespace ruby::audio {

// One audible layer: a decoded buffer placed on the composition's timeline.
// All times are COMPOSITION seconds, not buffer seconds.
struct AudioSource {
    const media::AudioBuffer* buffer = nullptr;

    double startSeconds = 0.0;   // when this layer begins in the composition
    double endSeconds = 0.0;     // when it stops
    double sourceOffset = 0.0;   // seconds into the buffer at startSeconds, i.e. the trim
    float gain = 1.0f;
};

// Plays a mix of decoded buffers and reports where the device actually is, so video can
// follow the audio clock instead of the wall clock (they drift otherwise).
class AudioOutput {
public:
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Null when no audio device could be opened. A legal state: the app runs silently.
    [[nodiscard]] static std::unique_ptr<AudioOutput> create();

    // Buffers are borrowed and must outlive this object. Safe to call while playing —
    // published to the audio thread via seqlock, not by stopping the device.
    void setSources(const std::vector<AudioSource>& sources);

    // Convenience for the single-clip case: one source starting at zero, running its own
    // length.
    void setBuffer(const media::AudioBuffer* buffer);

    void play(double fromSeconds);
    void stop();

    [[nodiscard]] bool playing() const noexcept { return playing_.load(); }

    // Position in COMPOSITION seconds, read from the device's own progress.
    [[nodiscard]] double position() const noexcept;

    // Audio thread only. `out` must be pre-zeroed; only samples that exist get written.
    // Always advances the clock, even with nothing to mix.
    void mix(float* out, std::uint32_t frames);

    // Fixed ceiling so the source list lives inline and copies without allocating.
    static constexpr int kMaxSources = 32;

private:
    class Impl;

    AudioOutput();

    std::unique_ptr<Impl> impl_;

    // Seqlock: UI thread writes, audio thread reads, no mutex (would risk dropouts in
    // the real-time callback). Writer brackets edits with an odd seq number; reader
    // copies and checks it didn't move, keeping the last good snapshot otherwise.
    std::atomic<std::uint32_t> seq_{0};
    AudioSource sources_[kMaxSources]{};
    int sourceCount_ = 0;

    // Audio thread only. Its private copy, refreshed when the sequence moves.
    AudioSource snapshot_[kMaxSources]{};
    int snapshotCount_ = 0;
    std::uint32_t snapshotSeq_ = 0;

    // Composition frames at the device rate, not an offset into any one buffer.
    std::atomic<std::uint64_t> cursor_{0};
    std::atomic<bool> playing_{false};

    friend class Impl;
};

}  // namespace ruby::audio
