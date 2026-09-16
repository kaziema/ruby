#include "ruby/core/Animation.h"

#include <algorithm>
#include <cmath>

namespace ruby::core {
namespace {

// Cubic bezier timing curve (CSS-style, control points (p1x,0)/(p2x,1)).
// Bisection, not Newton — curve is monotonic in x so it can't diverge.
double bezierSolve(double x, double p1x, double p2x) noexcept {
    const auto sampleX = [p1x, p2x](double t) {
        const double u = 1.0 - t;
        return 3.0 * u * u * t * p1x + 3.0 * u * t * t * p2x + t * t * t;
    };

    double lo = 0.0;
    double hi = 1.0;
    double t = x;
    for (int i = 0; i < 32; ++i) {
        const double err = sampleX(t) - x;
        if (std::fabs(err) < 1e-7) {
            break;
        }
        if (err > 0.0) {
            hi = t;
        } else {
            lo = t;
        }
        t = 0.5 * (lo + hi);
    }
    return t;
}

double bezierY(double t, double p1y, double p2y) noexcept {
    const double u = 1.0 - t;
    return 3.0 * u * u * t * p1y + 3.0 * u * t * t * p2y + t * t * t;
}

}  // namespace

Value lerp(const Value& a, const Value& b, double t) noexcept {
    Value out;
    out.count = std::max(a.count, b.count);
    for (std::size_t i = 0; i < 4; ++i) {
        out.c[i] = a.c[i] + (b.c[i] - a.c[i]) * t;
    }
    return out;
}

bool approxEqual(const Value& a, const Value& b, double eps) noexcept {
    if (a.count != b.count) {
        return false;
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(a.count); ++i) {
        if (std::fabs(a.c[i] - b.c[i]) > eps) {
            return false;
        }
    }
    return true;
}

double easeCurve(double t, double easeOut, double easeIn, double overshoot) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    easeOut = std::clamp(easeOut, 0.0, 1.0);
    easeIn = std::clamp(easeIn, 0.0, 1.0);

    // More ease-out drags the first control point right, holding the start value longer.
    const double solved = bezierSolve(t, easeOut, 1.0 - easeIn);
    double eased = bezierY(solved, 0.0, 1.0);

    if (overshoot > 0.0) {
        // Classic back-out: value sails past target, then settles.
        const double s = overshoot * 2.70158;
        const double u = eased - 1.0;
        eased = 1.0 + (s + 1.0) * u * u * u + s * u * u;
    }
    return eased;
}

std::size_t Property::addKey(const Keyframe& k, const TimeContext& ctx) {
    const double at = to_seconds(k.time, ctx);

    const auto pos = std::lower_bound(
        keys.begin(), keys.end(), at, [&ctx](const Keyframe& existing, double target) {
            return to_seconds(existing.time, ctx) < target;
        });

    if (pos != keys.end() && std::fabs(to_seconds(pos->time, ctx) - at) < 1e-9) {
        *pos = k;
        return static_cast<std::size_t>(std::distance(keys.begin(), pos));
    }

    const auto inserted = keys.insert(pos, k);
    return static_cast<std::size_t>(std::distance(keys.begin(), inserted));
}

// Clamps to the property's hard range. Applied on read, not write, so overshoot easing
// (e.g. a bounce past 100% opacity) renders clamped without flattening the stored curve.
Value Property::clamped(Value v) const noexcept {
    for (int i = 0; i < v.count; ++i) {
        v.c[static_cast<std::size_t>(i)] = range.clamp(v.c[static_cast<std::size_t>(i)]);
    }
    return v;
}

Value Property::evaluate(double seconds, const TimeContext& ctx) const {
    if (keys.empty()) {
        return clamped(staticValue);
    }
    if (keys.size() == 1) {
        return clamped(keys.front().value);
    }
    if (seconds <= to_seconds(keys.front().time, ctx)) {
        return clamped(keys.front().value);
    }
    if (seconds >= to_seconds(keys.back().time, ctx)) {
        return clamped(keys.back().value);
    }

    std::size_t i = 0;
    while (i + 1 < keys.size() && to_seconds(keys[i + 1].time, ctx) <= seconds) {
        ++i;
    }

    const Keyframe& a = keys[i];
    const Keyframe& b = keys[i + 1];

    if (a.interp == Interpolation::Hold) {
        return clamped(a.value);
    }

    const double t0 = to_seconds(a.time, ctx);
    const double t1 = to_seconds(b.time, ctx);
    const double span = t1 - t0;
    if (span <= 0.0) {
        return clamped(b.value);
    }

    const double raw = (seconds - t0) / span;
    const double t = (a.interp == Interpolation::Linear)
                         ? raw
                         : easeCurve(raw, a.easeOut, b.easeIn, b.overshoot);
    return clamped(lerp(a.value, b.value, t));
}

}  // namespace ruby::core
