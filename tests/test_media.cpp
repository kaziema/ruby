// Decodes real frames from a real file at RUBY_TEST_VIDEO; skips loudly if unset.

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <string>

#include "ruby/media/AudioDecoder.h"
#include "ruby/media/VideoDecoder.h"

using namespace ruby::media;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// A uniformly flat frame usually means colour conversion silently no-opped.
bool hasContent(const VideoFrame& frame) {
    std::uint8_t low = 255;
    std::uint8_t high = 0;
    for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
        low = std::min(low, frame.rgba[i]);
        high = std::max(high, frame.rgba[i]);
    }
    return high > low + 8;
}

}  // namespace

int main() {
    const char* path = std::getenv("RUBY_TEST_VIDEO");
    if (path == nullptr || *path == '\0') {
        std::puts("media: SKIPPED (set RUBY_TEST_VIDEO to a video file)");
        return EXIT_SUCCESS;
    }

    auto decoder = VideoDecoder::open(path);
    check(decoder != nullptr, "the file opens");
    if (decoder == nullptr) {
        std::fprintf(stderr, "could not open %s\n", path);
        return EXIT_FAILURE;
    }

    std::printf("opened %dx%d, %.3f fps, %.2fs\n", decoder->width(), decoder->height(),
                decoder->fps(), decoder->duration());

    check(decoder->width() > 0 && decoder->height() > 0, "the file reports its size");
    check(decoder->fps() > 0.0, "the file reports a frame rate");
    check(decoder->duration() > 0.0, "the file reports a duration");

    const VideoFrame* first = decoder->frameAt(1.0);
    check(first != nullptr, "a frame decodes at 1s");
    if (first != nullptr) {
        check(first->valid(), "the frame has pixels");
        check(first->width == decoder->width(), "frame width matches the stream");
        check(first->rgba.size() ==
                  static_cast<std::size_t>(first->width) *
                      static_cast<std::size_t>(first->height) * 4,
              "the buffer is tightly packed RGBA");
        check(hasContent(*first), "the frame is not uniformly flat");
    }

    // Seek backward, which a naive decoder gets wrong without a keyframe rewind.
    const VideoFrame* later = decoder->frameAt(3.0);
    check(later != nullptr && later->valid(), "a later frame decodes");
    const double laterPts = (later != nullptr) ? later->pts : -1.0;

    const VideoFrame* back = decoder->frameAt(0.5);
    check(back != nullptr && back->valid(), "seeking backwards still decodes");
    if (back != nullptr) {
        check(back->pts < laterPts, "the rewound frame is genuinely earlier");
    }

    // Asking for the same instant twice should not re-seek or change the answer.
    const VideoFrame* again = decoder->frameAt(0.5);
    check(again != nullptr && again->valid(), "re-requesting the same time works");

    // --- audio ---------------------------------------------------------------
    auto audio = AudioDecoder::decode(path);
    if (!audio.has_value()) {
        std::puts("no audio stream in this file; skipping the audio checks");
    } else {
        std::printf("audio %d ch @ %d Hz, %.2fs\n", audio->channels, audio->sampleRate,
                    audio->duration());

        check(audio->valid(), "the audio buffer is usable");
        check(audio->sampleRate == 48000, "audio is resampled to the target rate");
        check(audio->channels >= 1 && audio->channels <= 2, "channels are capped at stereo");

        check(std::fabs(audio->duration() - decoder->duration()) < 0.5,
              "audio and video durations agree");

        float loudest = 0.0f;
        for (float v : audio->samples) {
            loudest = std::max(loudest, std::fabs(v));
        }
        check(loudest > 0.01f, "the audio is not silence");
        check(loudest <= 1.5f, "samples are in a sane float range, not raw integers");

        const WaveformPeaks wave = AudioDecoder::peaks(*audio, 100.0);
        check(!wave.empty(), "peaks are computed");
        check(wave.low.size() == wave.high.size(), "every bucket has both bounds");
        check(std::fabs(static_cast<double>(wave.low.size()) / 100.0 - audio->duration())
                  < 0.2,
              "the number of buckets matches the duration");

        bool ordered = true;
        float peak = 0.0f;
        for (std::size_t i = 0; i < wave.low.size(); ++i) {
            if (wave.low[i] > wave.high[i]) ordered = false;
            peak = std::max(peak, wave.high[i]);
        }
        check(ordered, "every bucket's low is below its high");
        check(peak > 0.01f, "the waveform has something in it");
    }

    check(!AudioDecoder::decode("/definitely/not/a/file.wav").has_value(),
          "a missing file returns nothing rather than an empty buffer");

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("media: all checks passed");
    return EXIT_SUCCESS;
}
