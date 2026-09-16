// Time units and spatial units.
// Plain asserts, no framework. Kept deliberately dependency-free.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>

#include "ruby/core/Units.h"

using namespace ruby::core;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void check_near(double a, double b, const char* what) {
    if (std::fabs(a - b) > 1e-9) {
        std::fprintf(stderr, "FAIL: %s (got %.10f, want %.10f)\n", what, a, b);
        ++failures;
    }
}

// Frames-authored time changes duration when the frame rate changes; seconds don't.
void frames_are_not_a_unit_of_time() {
    const TimeContext at30{30.0, 120.0, false};
    const TimeContext at60{60.0, 120.0, false};

    const TimeValue three_frames = TimeValue::frames(3.0);
    check_near(to_seconds(three_frames, at30), 0.1, "3 frames @30fps == 100ms");
    check_near(to_seconds(three_frames, at60), 0.05, "3 frames @60fps == 50ms");

    const TimeValue hundred_ms = TimeValue::seconds(0.1);
    check_near(to_seconds(hundred_ms, at30), 0.1, "100ms @30fps");
    check_near(to_seconds(hundred_ms, at60), 0.1, "100ms @60fps");
    check_near(to_frames(hundred_ms, at30), 3.0, "100ms is 3 frames @30fps");
    check_near(to_frames(hundred_ms, at60), 6.0, "100ms is 6 frames @60fps");
}

// Half a beat is a different duration at every BPM; music-synced time stores beats.
void beats_survive_tempo_changes() {
    const TimeContext slow{30.0, 90.0, true};
    const TimeContext fast{30.0, 174.0, true};

    const TimeValue half_beat = TimeValue::beats(0.5);
    check_near(to_seconds(half_beat, slow), 30.0 / 90.0, "half beat @90bpm");
    check_near(to_seconds(half_beat, fast), 30.0 / 174.0, "half beat @174bpm");

    const TimeValue one_bar = TimeValue::beats(4.0);
    check_near(to_seconds(one_bar, slow), 240.0 / 90.0, "one bar @90bpm");
}

// Frames mode must pass through exactly, not round-trip through seconds.
void frames_mode_passes_through_exactly() {
    const TimeContext odd{23.976, 137.0, true};
    check_near(to_frames(TimeValue::frames(3.0), odd), 3.0, "3 frames stays 3 frames");
}

// Pixel-stored values don't survive a resolution change; declared units do.
void spatial_units_survive_resolution_changes() {
    const FrameGeometry landscape{1920, 1080};
    const FrameGeometry vertical{1080, 1920};

    // Pixels: same number, visually very different relative to the frame.
    check_near(resolve_spatial(50.0, SpatialUnit::Px, landscape), 50.0, "px landscape");
    check_near(resolve_spatial(50.0, SpatialUnit::Px, vertical), 50.0, "px vertical");

    // Diagonal percent: identical relative size, because the diagonal is the same.
    const double a = resolve_spatial(2.5, SpatialUnit::PercentOfDiagonal, landscape);
    const double b = resolve_spatial(2.5, SpatialUnit::PercentOfDiagonal, vertical);
    check_near(a, b, "percent_of_diagonal is orientation independent");
    check(a > 0.0, "diagonal resolves to something positive");

    // Width percent tracks the width, which is the point.
    check_near(resolve_spatial(10.0, SpatialUnit::PercentOfWidth, landscape), 192.0,
               "10% of 1920");
    check_near(resolve_spatial(10.0, SpatialUnit::PercentOfWidth, vertical), 108.0,
               "10% of 1080");

    // Round trip.
    for (const SpatialUnit u : {SpatialUnit::Px, SpatialUnit::PercentOfWidth,
                                SpatialUnit::PercentOfHeight, SpatialUnit::PercentOfDiagonal,
                                SpatialUnit::Degrees, SpatialUnit::Percent,
                                SpatialUnit::Normalized}) {
        const double px = resolve_spatial(37.5, u, vertical);
        check_near(store_spatial(px, u, vertical), 37.5, "spatial round trip");
    }
}

// Suffix is what the UI shows after the number.
void units_carry_a_display_suffix() {
    check(std::string(unitSuffix(SpatialUnit::Percent)) == "%", "percent shows %");
    check(std::string(unitSuffix(SpatialUnit::PercentOfWidth)) == "%",
          "percent-of-width shows % too");
    check(std::string(unitSuffix(SpatialUnit::Degrees)) != "", "degrees has a suffix");
    check(std::string(unitSuffix(SpatialUnit::Normalized)) == "", "normalized has none");
}

void degenerate_contexts_do_not_explode() {
    const TimeContext zero_fps{0.0, 0.0, false};
    check_near(to_seconds(TimeValue::frames(5.0), zero_fps), 0.0, "zero fps is not a crash");
    check_near(to_seconds(TimeValue::beats(5.0), zero_fps), 0.0, "zero bpm is not a crash");

    const FrameGeometry empty{0, 0};
    check_near(store_spatial(10.0, SpatialUnit::PercentOfWidth, empty), 0.0,
               "zero width is not a crash");
    check_near(store_spatial(10.0, SpatialUnit::PercentOfDiagonal, empty), 0.0,
               "zero diagonal is not a crash");
}

}  // namespace

int main() {
    frames_are_not_a_unit_of_time();
    beats_survive_tempo_changes();
    frames_mode_passes_through_exactly();
    spatial_units_survive_resolution_changes();
    units_carry_a_display_suffix();
    degenerate_contexts_do_not_explode();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("units: all checks passed");
    return EXIT_SUCCESS;
}
