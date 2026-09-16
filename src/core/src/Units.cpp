#include "ruby/core/Units.h"

#include <cmath>

namespace ruby::core {

double to_seconds(TimeValue t, const TimeContext& ctx) noexcept {
    switch (t.mode) {
        case TimeMode::Seconds:
            return t.value;
        case TimeMode::Frames:
            return ctx.fps > 0.0 ? t.value / ctx.fps : 0.0;
        case TimeMode::Beats:
            return ctx.bpm > 0.0 ? t.value * (60.0 / ctx.bpm) : 0.0;
    }
    return 0.0;
}

double to_frames(TimeValue t, const TimeContext& ctx) noexcept {
    if (t.mode == TimeMode::Frames) {
        return t.value;
    }
    return to_seconds(t, ctx) * ctx.fps;
}

double diagonal(const FrameGeometry& g) noexcept {
    const double w = static_cast<double>(g.width);
    const double h = static_cast<double>(g.height);
    return std::sqrt(w * w + h * h);
}

double resolve_spatial(double value, SpatialUnit unit, const FrameGeometry& g) noexcept {
    switch (unit) {
        case SpatialUnit::Px:
            return value;
        case SpatialUnit::PercentOfWidth:
            return value * 0.01 * static_cast<double>(g.width);
        case SpatialUnit::PercentOfHeight:
            return value * 0.01 * static_cast<double>(g.height);
        case SpatialUnit::PercentOfDiagonal:
            return value * 0.01 * diagonal(g);
        case SpatialUnit::Degrees:
        case SpatialUnit::Percent:
        case SpatialUnit::Normalized:
            return value;
    }
    return value;
}

double store_spatial(double pixels, SpatialUnit unit, const FrameGeometry& g) noexcept {
    switch (unit) {
        case SpatialUnit::Px:
            return pixels;
        case SpatialUnit::PercentOfWidth:
            return g.width > 0 ? pixels * 100.0 / static_cast<double>(g.width) : 0.0;
        case SpatialUnit::PercentOfHeight:
            return g.height > 0 ? pixels * 100.0 / static_cast<double>(g.height) : 0.0;
        case SpatialUnit::PercentOfDiagonal: {
            const double d = diagonal(g);
            return d > 0.0 ? pixels * 100.0 / d : 0.0;
        }
        case SpatialUnit::Degrees:
        case SpatialUnit::Percent:
        case SpatialUnit::Normalized:
            return pixels;
    }
    return pixels;
}

const char* unitSuffix(SpatialUnit unit) noexcept {
    switch (unit) {
        case SpatialUnit::PercentOfWidth:
        case SpatialUnit::PercentOfHeight:
        case SpatialUnit::PercentOfDiagonal:
        case SpatialUnit::Percent:
            return "%";
        case SpatialUnit::Degrees:
            return "\u00b0";
        case SpatialUnit::Px:
            return " px";
        case SpatialUnit::Normalized:
            return "";
        case SpatialUnit::Decibels:
            return " dB";
    }
    return "";
}

namespace {
constexpr double kSilenceFloorDb = -96.0;
}

double linearToDecibels(double linear) noexcept {
    if (!(linear > 0.0)) {
        return kSilenceFloorDb;
    }
    const double db = 20.0 * std::log10(linear);
    return db < kSilenceFloorDb ? kSilenceFloorDb : db;
}

double decibelsToLinear(double decibels) noexcept {
    if (decibels <= kSilenceFloorDb) {
        return 0.0;
    }
    return std::pow(10.0, decibels / 20.0);
}

}  // namespace ruby::core
