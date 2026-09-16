#pragma once

#include <cstdint>
#include <functional>

#include "ruby/core/Document.h"

namespace ruby::core {

// 2D affine transform, the six meaningful values of a 3x3:
//
//     | a  c  tx |
//     | b  d  ty |
//     | 0  0  1  |
//
// Narrow form keeps parent-chain composition cheap (10 multiplies vs. 64 for a 4x4);
// the compositor widens it for the shader.
struct Transform2D {
    double a = 1.0, b = 0.0;
    double c = 0.0, d = 1.0;
    double tx = 0.0, ty = 0.0;

    [[nodiscard]] static Transform2D identity() noexcept { return {}; }
    [[nodiscard]] static Transform2D translate(double x, double y) noexcept;
    [[nodiscard]] static Transform2D scale(double x, double y) noexcept;
    [[nodiscard]] static Transform2D rotate(double degrees) noexcept;

    // `a.then(b)` applies a first, then b — maps p to b(a(p)).
    [[nodiscard]] Transform2D then(const Transform2D& outer) const noexcept;

    [[nodiscard]] double applyX(double x, double y) const noexcept;
    [[nodiscard]] double applyY(double x, double y) const noexcept;

    // Undoes this transform; identity if degenerate (e.g. zero scale — no answer to
    // where a pixel came from). Needed to convert a distance from composition space
    // into a parent's space.
    [[nodiscard]] Transform2D inverse() const noexcept;
};

// An axis-aligned box in composition pixels.
struct Bounds {
    double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;

    [[nodiscard]] double width() const noexcept { return right - left; }
    [[nodiscard]] double height() const noexcept { return bottom - top; }
    [[nodiscard]] double centerX() const noexcept { return (left + right) / 2.0; }
    [[nodiscard]] double centerY() const noexcept { return (top + bottom) / 2.0; }
};

// Safety limit, not a style one — nothing stops a file/paste/script from creating a
// parent cycle, and walking one uncapped hangs the render path.
inline constexpr int kMaxParentDepth = 32;

// Layer size in composition pixels, before scale. Core can't compute this (decoder/
// rasterizer live outside this module), so the caller supplies it per layer.
struct LayerSize {
    double width = 0.0;
    double height = 0.0;
};
using SizeOf = std::function<LayerSize(const Layer&)>;

// A layer's own transform, in composition units (a fraction of the frame for position,
// percent for scale, degrees for rotation), before any parent is applied.
[[nodiscard]] Transform2D layerTransform(const Layer& layer, double seconds,
                                         const TimeContext& ctx, double compWidth,
                                         double compHeight, const LayerSize& size);

// Same, with every parent applied up the chain. Bounded by kMaxParentDepth and stops
// on a repeat — a cycle renders unparented rather than hanging. Each parent's anchor
// uses ITS OWN size via `sizeOf`, not the child's.
[[nodiscard]] Transform2D resolvedTransform(const Composition& comp, const Layer& layer,
                                            double seconds, const TimeContext& ctx,
                                            double compWidth, double compHeight,
                                            const SizeOf& sizeOf);

// Axis-aligned box around the layer's four corners after its full transform (scale,
// rotation, anchor, parents) — a rotated layer reports the containing box, not its
// tilted rectangle. Bounds at this moment only; an animated layer's box changes
// every frame.
[[nodiscard]] Bounds layerBounds(const Composition& comp, const Layer& layer,
                                 double seconds, const TimeContext& ctx, double compWidth,
                                 double compHeight, const SizeOf& sizeOf);

// Whether the parent chain cycles back to itself or exceeds the cap. Used to refuse
// bad parenting before it's stored.
[[nodiscard]] bool hasParentCycle(const Composition& comp, LayerId layer) noexcept;

// Whether `candidate` may become `layer`'s parent. False when it is the layer itself, or
// when the layer is already somewhere above the candidate.
[[nodiscard]] bool canParentTo(const Composition& comp, LayerId layer,
                               LayerId candidate) noexcept;

}  // namespace ruby::core
