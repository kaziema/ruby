#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruby/core/Units.h"

namespace ruby::core {

// --- Value -------------------------------------------------------------------
//
// One representation for every animatable property: up to 4 doubles + a count.
// Covers scalars, pairs, triples, RGBA. Simpler to interpolate than a variant.

struct Value {
    std::array<double, 4> c{};
    int count = 1;

    static constexpr Value scalar(double v) { return {{v, 0, 0, 0}, 1}; }
    static constexpr Value vec2(double x, double y) { return {{x, y, 0, 0}, 2}; }
    static constexpr Value vec3(double x, double y, double z) { return {{x, y, z, 0}, 3}; }
    static constexpr Value rgba(double r, double g, double b, double a) {
        return {{r, g, b, a}, 4};
    }

    [[nodiscard]] double x() const { return c[0]; }
    [[nodiscard]] double y() const { return c[1]; }
};

[[nodiscard]] Value lerp(const Value& a, const Value& b, double t) noexcept;
[[nodiscard]] bool approxEqual(const Value& a, const Value& b, double eps = 1e-9) noexcept;

// --- Keyframes ---------------------------------------------------------------
//
// Times are TimeValue, so a keyframe authored in beats stays in beats across tempo
// changes. Easing: ease-out influence leaving a key, ease-in arriving at the next,
// plus an overshoot amount.

enum class Interpolation {
    Linear,
    Bezier,
    Hold,  // steps to the next value, no interpolation
};

struct Keyframe {
    TimeValue time = TimeValue::seconds(0.0);
    Value value;
    Interpolation interp = Interpolation::Bezier;

    double easeOut = 0.0;    // 0..1 influence leaving this key
    double easeIn = 0.0;     // 0..1 influence arriving at this key
    double overshoot = 0.0;  // 0..1 overshoot amount (back-out easing)
};

// Maps normalized 0..1 time to eased 0..1; exposed so the graph editor can draw it.
[[nodiscard]] double easeCurve(double t, double easeOut, double easeIn,
                               double overshoot) noexcept;

// --- Property ----------------------------------------------------------------

using PropertyId = std::uint64_t;

struct Property {
    PropertyId id = 0;
    std::string key;    // stable, matches the effect schema's parameter key
    std::string label;  // display only
    // Which collapsible group this row sits under in the inspector: "Transform", or the
    // display name of the effect instance that owns it.
    std::string group = "Transform";
    SpatialUnit unit = SpatialUnit::Normalized;

    // Copied from ParamSpec on instantiation, or set directly for transform properties
    // (no schema). Duplicated here, like unit/label, since inspector/compositor only
    // ever see the runtime property.
    ParamRange range;

    Value staticValue;           // used when there are no keyframes
    std::vector<Keyframe> keys;  // kept sorted by resolved time
    std::optional<std::string> expression;
    bool expanded = false;  // twirled open in the timeline

    [[nodiscard]] bool animated() const noexcept { return !keys.empty(); }

    // Clamps every component to `range`. Public so expressions land in the same range
    // a keyframe would.
    [[nodiscard]] Value clamped(Value v) const noexcept;

    // Inserts in time order, returns the index. Replaces an existing key at the same time.
    std::size_t addKey(const Keyframe& k, const TimeContext& ctx);

    // Value at a wall-clock second; clamps to the first/last key outside their range
    // (matches AE).
    [[nodiscard]] Value evaluate(double seconds, const TimeContext& ctx) const;
};

}  // namespace ruby::core
