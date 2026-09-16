#pragma once

#include <cstdint>
#include <optional>

namespace ruby::core {

// --- Parameter range ---------------------------------------------------------
//
// A hard limit and a slider range are not the same thing: the hard limit is what's
// legal, the slider is where the useful values live. Many params have a floor but no
// ceiling ("0 or greater"), yet the slider still needs an end.
//
//     Blur radius:  floor 0, no ceiling, slider ends at 20.
//     Opacity:      hard 0 to 100, slider the same.
//     Scale:        no limit either way (negative flips), slider -200 to 400.
struct ParamRange {
    // Absent means unbounded on that side; out-of-range values are clamped regardless
    // of source (drag, typed, keyframe, expression).
    std::optional<double> minimum;
    std::optional<double> maximum;

    // Where a slider/scrub gesture runs from and to. Inside the hard range when there
    // is one, usually narrower when there isn't.
    double slider_min = 0.0;
    double slider_max = 1.0;

    [[nodiscard]] constexpr double clamp(double v) const noexcept {
        if (minimum.has_value() && v < *minimum) return *minimum;
        if (maximum.has_value() && v > *maximum) return *maximum;
        return v;
    }

    // Value moved per pixel of horizontal drag, derived from the slider span (not the
    // unit) so ranges scrub proportionally without manual tuning.
    [[nodiscard]] constexpr double dragStep() const noexcept {
        const double span = slider_max - slider_min;
        return (span > 0.0 ? span : 1.0) / 260.0;  // ~260px to cross the useful range
    }

    // The common shapes, named. Reads better at a declaration site than four fields.
    [[nodiscard]] static constexpr ParamRange atLeast(double lo, double sliderTop) noexcept {
        return {lo, std::nullopt, lo, sliderTop};
    }
    [[nodiscard]] static constexpr ParamRange between(double lo, double hi) noexcept {
        return {lo, hi, lo, hi};
    }
    [[nodiscard]] static constexpr ParamRange unbounded(double sliderLo,
                                                        double sliderHi) noexcept {
        return {std::nullopt, std::nullopt, sliderLo, sliderHi};
    }
};

// --- Time -------------------------------------------------------------------
//
// Stored in beats or seconds; frames only if the author opts in explicitly. A frame
// isn't a unit of time, it's time / framerate, so "3 frames" plays twice as fast at
// 60fps as at 30fps with no warning. Beats exist because a fixed beat fraction is a
// different duration at different tempos; anything meant to land on the music must
// be stored in beats. Frames stay available for effects genuinely defined by discrete
// frames (stutter, strobe, posterize-time, anime "on 2s").

enum class TimeMode {
    Beats,
    Seconds,
    Frames,
};

struct TimeValue {
    TimeMode mode = TimeMode::Seconds;
    double value = 0.0;

    static constexpr TimeValue beats(double v) { return {TimeMode::Beats, v}; }
    static constexpr TimeValue seconds(double v) { return {TimeMode::Seconds, v}; }
    static constexpr TimeValue frames(double v) { return {TimeMode::Frames, v}; }
};

// Everything needed to resolve a TimeValue into wall-clock seconds.
struct TimeContext {
    double fps = 30.0;
    double bpm = 120.0;      // fallback tempo, used when has_beat_map is false
    bool has_beat_map = false;
};

// Resolves to seconds; beats resolve against the project tempo.
// OPEN QUESTION: with no beat map, falls back to TimeContext::bpm rather than the
// seconds value the preset author previewed at. Not decided which is right.
double to_seconds(TimeValue t, const TimeContext& ctx) noexcept;

// Resolve to a frame index; Frames mode passes through exactly (the point of opting in).
double to_frames(TimeValue t, const TimeContext& ctx) noexcept;

// --- Space ------------------------------------------------------------------
//
// Every numeric parameter declares its unit. Storing blur radii in raw pixels breaks
// presets moved between resolutions; percent_of_diagonal keeps them portable.

enum class SpatialUnit {
    Px,
    PercentOfWidth,
    PercentOfHeight,
    PercentOfDiagonal,
    Degrees,
    // Resolution-independent percentage (scale, opacity, effect amounts) — distinct
    // from PercentOfWidth/etc., which resolve against the frame.
    Percent,
    Normalized,  // unitless, no suffix
    // Display-only: the stored Value is linear amplitude/gain. Converted to dB for
    // display and back on edit, so keyframe interpolation stays linear-amplitude
    // (a correct fade) instead of linear-in-dB (audibly wrong, AE's own documented flaw).
    Decibels,
};

// Suffix shown after the number in the UI. Empty for units that do not take one.
[[nodiscard]] const char* unitSuffix(SpatialUnit unit) noexcept;

// dB <-> linear amplitude, with -96dB as the silence floor (16-bit noise floor, matches
// After Effects). linearToDecibels(0) and anything below the floor returns -96.
[[nodiscard]] double linearToDecibels(double linear) noexcept;
[[nodiscard]] double decibelsToLinear(double decibels) noexcept;

struct FrameGeometry {
    int width = 1920;
    int height = 1080;
};

double diagonal(const FrameGeometry& g) noexcept;

// Resolve a parameter's stored value into pixels for the given frame geometry.
// Degrees and Normalized pass through unchanged; they are resolution independent.
double resolve_spatial(double value, SpatialUnit unit, const FrameGeometry& g) noexcept;

// Inverse: convert a pixel measurement back into the parameter's declared unit.
// Needed when the UI reports a drag in pixels and we store it in the param's unit.
double store_spatial(double pixels, SpatialUnit unit, const FrameGeometry& g) noexcept;

}  // namespace ruby::core
