// Waveform peak pyramid and its on-disk cache. Uses a synthesised AudioBuffer, no FFmpeg.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "ruby/media/PeakCache.h"

using namespace ruby::media;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void checkNear(double a, double b, const char* what, double eps = 1e-6) {
    if (std::fabs(a - b) > eps) {
        std::fprintf(stderr, "FAIL: %s (got %.8f, want %.8f)\n", what, a, b);
        ++failures;
    }
}

std::string tempFile(const char* leaf) {
    return (std::filesystem::temp_directory_path() / leaf).string();
}

// A sine, so every bucket has real content and the extremes are known.
AudioBuffer tone(double seconds, int rate = 48000, int channels = 1) {
    AudioBuffer buffer;
    buffer.sampleRate = rate;
    buffer.channels = channels;
    const auto frames = static_cast<std::size_t>(seconds * rate);
    buffer.samples.resize(frames * static_cast<std::size_t>(channels));
    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(
            std::sin(2.0 * 3.14159265358979 * 440.0 * static_cast<double>(f) / rate));
        for (int c = 0; c < channels; ++c) {
            buffer.samples[f * static_cast<std::size_t>(channels) +
                           static_cast<std::size_t>(c)] = v;
        }
    }
    return buffer;
}

void the_base_level_is_the_documented_rate() {
    const PeakPyramid pyramid = PeakPyramid::build(tone(4.0));
    check(!pyramid.empty(), "a four second tone produces a pyramid");
    checkNear(pyramid.levels().front().bucketsPerSecond, kBasePeaksPerSecond,
              "the finest level runs at the base rate");
    checkNear(pyramid.duration(), 4.0, "duration came through", 1e-3);

    // 4s at 150/sec is 600 buckets, which is over the 64 floor, so it must have climbed.
    check(pyramid.levels().size() > 1, "and coarser levels were built above it");
}

void each_level_is_a_quarter_of_the_one_below() {
    const PeakPyramid pyramid = PeakPyramid::build(tone(30.0));
    const auto& levels = pyramid.levels();
    check(levels.size() >= 3, "thirty seconds is deep enough to check the ratio");

    for (std::size_t i = 1; i < levels.size(); ++i) {
        checkNear(levels[i].bucketsPerSecond, levels[i - 1].bucketsPerSecond / 4.0,
                  "each level is a quarter the rate of the previous one");
        check(levels[i].count() < levels[i - 1].count(), "and holds fewer buckets");
    }
    check(levels.back().count() <= 64 * 4,
          "climbing stops once a level is too small to show structure");
}

void summarising_a_summary_is_exact_for_min_max() {
    const PeakPyramid pyramid = PeakPyramid::build(tone(8.0));
    const auto& levels = pyramid.levels();
    check(levels.size() >= 2, "need two levels to compare");

    const PeakLevel& fine = levels[0];
    const PeakLevel& coarse = levels[1];

    // Coarse buckets must be exact extremes of the fine ones, or the waveform shape drifts on zoom.
    for (std::size_t b = 0; b < coarse.count(); ++b) {
        const std::size_t begin = b * 4;
        const std::size_t end = std::min(begin + 4, fine.count());
        float lo = fine.low[begin];
        float hi = fine.high[begin];
        for (std::size_t i = begin + 1; i < end; ++i) {
            lo = std::min(lo, fine.low[i]);
            hi = std::max(hi, fine.high[i]);
        }
        if (std::fabs(static_cast<double>(coarse.low[b]) - static_cast<double>(lo)) > 1e-9 ||
            std::fabs(static_cast<double>(coarse.high[b]) - static_cast<double>(hi)) >
                1e-9) {
            std::fprintf(stderr, "FAIL: coarse bucket %zu is not the extremes below it\n", b);
            ++failures;
            return;
        }
    }
}

void the_level_chosen_matches_the_zoom() {
    const PeakPyramid pyramid = PeakPyramid::build(tone(60.0));

    // Over-zoomed request must still return a level, the finest one.
    const PeakLevel* fine = pyramid.levelFor(0.001);
    check(fine != nullptr, "an over-zoomed request still returns a level");
    checkNear(fine->bucketsPerSecond, kBasePeaksPerSecond, "and it is the finest one");

    // Zoomed out: coarser level avoids reading far more data than the pixel needs.
    const PeakLevel* coarse = pyramid.levelFor(1.0);
    check(coarse != nullptr, "a zoomed out request returns a level");
    check(coarse->bucketsPerSecond < kBasePeaksPerSecond,
          "and it is coarser than the base rate");
    check(coarse->bucketsPerSecond >= 1.0,
          "but still at least one bucket per pixel, so transients survive");

    // Monotonic: zooming out must never hand back a finer level than zooming in did.
    check(pyramid.levelFor(1.0)->bucketsPerSecond <=
              pyramid.levelFor(0.01)->bucketsPerSecond,
          "coarser zoom never selects a finer level");
}

void an_empty_buffer_produces_nothing() {
    const PeakPyramid pyramid = PeakPyramid::build(AudioBuffer{});
    check(pyramid.empty(), "an invalid buffer builds no pyramid");
    check(pyramid.levelFor(0.1) == nullptr, "and has no level to draw");
    check(!pyramid.save(tempFile("ruby_peaks_empty.rbypeak")),
          "and refuses to write an empty cache");
}

void a_pyramid_survives_a_round_trip() {
    const std::string file = tempFile("ruby_peaks_roundtrip.rbypeak");
    std::filesystem::remove(file);

    const PeakPyramid written = PeakPyramid::build(tone(12.0, 48000, 2));
    check(written.save(file), "saved");

    PeakPyramid read;
    check(read.load(file), "loaded");
    check(read.levels().size() == written.levels().size(), "same number of levels");
    checkNear(read.duration(), written.duration(), "same duration", 1e-9);

    for (std::size_t i = 0; i < read.levels().size(); ++i) {
        checkNear(read.levels()[i].bucketsPerSecond, written.levels()[i].bucketsPerSecond,
                  "level rate survived");
        check(read.levels()[i].low == written.levels()[i].low, "lows survived exactly");
        check(read.levels()[i].high == written.levels()[i].high, "highs survived exactly");
    }
    std::filesystem::remove(file);
}

// A bad cache is a miss, not an error; it can always be rebuilt from the source samples.
void a_bad_cache_is_a_miss_not_an_error() {
    PeakPyramid pyramid;
    check(!pyramid.load(tempFile("ruby_peaks_does_not_exist.rbypeak")),
          "a missing cache file simply misses");

    const std::string junk = tempFile("ruby_peaks_junk.rbypeak");
    {
        std::ofstream out(junk, std::ios::binary | std::ios::trunc);
        out << "definitely not a peak file";
    }
    check(!pyramid.load(junk), "a file with the wrong magic misses");
    check(pyramid.empty(), "and leaves nothing half-loaded behind");
    std::filesystem::remove(junk);

    // Truncated mid-level: the header promises data that is not there.
    const std::string cut = tempFile("ruby_peaks_truncated.rbypeak");
    {
        const PeakPyramid full = PeakPyramid::build(tone(5.0));
        full.save(cut);
        const auto size = std::filesystem::file_size(cut);
        std::filesystem::resize_file(cut, size / 2);
    }
    check(!pyramid.load(cut), "a truncated cache misses rather than reading garbage");
    check(pyramid.empty(), "and leaves nothing behind");
    std::filesystem::remove(cut);
}

// Cache key must not rely on path alone; a re-exported clip keeps its name.
void the_cache_key_notices_a_changed_file() {
    const std::string media = tempFile("ruby_peaks_media.bin");
    {
        std::ofstream out(media, std::ios::binary | std::ios::trunc);
        out << "first version";
    }
    const std::string before = peakCachePath("/cache", media);

    {
        std::ofstream out(media, std::ios::binary | std::ios::trunc);
        out << "a second version, of a different length entirely";
    }
    const std::string after = peakCachePath("/cache", media);

    check(before != after, "the same path with different contents keys differently");
    check(before.rfind("/cache/", 0) == 0, "the key lands in the directory it was given");
    std::filesystem::remove(media);
}

}  // namespace

int main() {
    the_base_level_is_the_documented_rate();
    each_level_is_a_quarter_of_the_one_below();
    summarising_a_summary_is_exact_for_min_max();
    the_level_chosen_matches_the_zoom();
    an_empty_buffer_produces_nothing();
    a_pyramid_survives_a_round_trip();
    a_bad_cache_is_a_miss_not_an_error();
    the_cache_key_notices_a_changed_file();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("peakcache: all checks passed");
    return EXIT_SUCCESS;
}
