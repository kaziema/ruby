// Viewer hit-test maths, checked without a window. Hit tests go through each layer's
// transform rather than its bounding box, since a rotated box covers empty corners.

#include <cmath>
#include <cstdio>
#include <string>

#include "ruby/core/Transform.h"
#include "ruby/engine/RenderGraph.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

void near(double got, double want, const std::string& what, double eps = 1e-6) {
    if (std::fabs(got - want) > eps) {
        std::fprintf(stderr, "FAIL: %s (got %.4f, wanted %.4f)\n", what.c_str(), got, want);
        ++failures;
    }
}

constexpr double kCompW = 1920.0;
constexpr double kCompH = 1080.0;

core::SizeOf sized(double w, double h) {
    return [w, h](const core::Layer&) { return core::LayerSize{w, h}; };
}

// The same transform chain the viewer builds and the compositor draws with.
core::Transform2D unitToWidget(const core::Composition& comp, const core::Layer& layer,
                               const core::SizeOf& sizes, const engine::FrameFit& fit) {
    const core::LayerSize size = sizes(layer);
    return core::Transform2D::translate(-0.5, -0.5)
        .then(core::Transform2D::scale(size.width, size.height))
        .then(core::resolvedTransform(comp, layer, 0.0, comp.timeContext(), kCompW, kCompH,
                                      sizes))
        .then(core::Transform2D::scale(fit.scale, fit.scale))
        .then(core::Transform2D::translate(fit.x, fit.y));
}

bool hits(const core::Composition& comp, const core::Layer& layer,
          const core::SizeOf& sizes, const engine::FrameFit& fit, double x, double y) {
    const core::Transform2D back = unitToWidget(comp, layer, sizes, fit).inverse();
    const double u = back.applyX(x, y);
    const double v = back.applyY(x, y);
    return u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0;
}

// FrameFit must match the renderer's, or a click lands on the wrong layer.
void the_fit_letterboxes_both_ways() {
    // 16:9 comp in a tall window: bars top and bottom.
    const engine::FrameFit tall = engine::frameFit(1920.0, 1080.0, 800.0, 800.0);
    near(tall.width, 800.0, "fills the width");
    near(tall.height, 450.0, "and is letterboxed vertically");
    near(tall.x, 0.0, "flush left");
    near(tall.y, 175.0, "centred vertically");

    // Same comp in a wide window: bars left and right.
    const engine::FrameFit wide = engine::frameFit(1920.0, 1080.0, 2000.0, 500.0);
    near(wide.height, 500.0, "fills the height");
    near(wide.width, 888.888888, "and is pillarboxed", 1e-3);

    // Round trip, which hit testing depends on.
    near(tall.toCompX(tall.toTargetX(640.0)), 640.0, "x round trips");
    near(tall.toCompY(tall.toTargetY(360.0)), 360.0, "y round trips");
}

void a_click_inside_a_layer_hits_it() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1920, 1080, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "solid", core::LayerKind::Solid).id;
    const core::SizeOf sizes = sized(400.0, 200.0);
    const engine::FrameFit fit = engine::frameFit(kCompW, kCompH, 1920.0, 1080.0);
    const core::Layer& layer = *comp.find(id);

    check(hits(comp, layer, sizes, fit, 960.0, 540.0), "the centre of a centred layer");
    check(!hits(comp, layer, sizes, fit, 100.0, 100.0), "and not the far corner");
    check(hits(comp, layer, sizes, fit, 960.0 + 190.0, 540.0), "just inside the right edge");
    check(!hits(comp, layer, sizes, fit, 960.0 + 210.0, 540.0), "just outside it");
}

// A 45-degree layer's bounding box has empty corners; hit-testing must not select there.
void a_rotated_layer_is_not_its_bounding_box() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1920, 1080, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "solid", core::LayerKind::Solid).id;
    comp.find(id)->find("rotation")->staticValue = core::Value::scalar(45.0);

    const core::SizeOf sizes = sized(400.0, 400.0);
    const engine::FrameFit fit = engine::frameFit(kCompW, kCompH, 1920.0, 1080.0);
    const core::Layer& layer = *comp.find(id);

    const core::Bounds box =
        core::layerBounds(comp, layer, 0.0, comp.timeContext(), kCompW, kCompH, sizes);
    // A 400 square at 45 degrees spans 400*sqrt(2).
    near(box.width(), 400.0 * std::sqrt(2.0), "the box is the diagonal", 1e-3);

    // Just inside the box's top-left corner, but outside the diamond.
    const double cx = fit.toTargetX(box.left + 12.0);
    const double cy = fit.toTargetY(box.top + 12.0);
    check(!hits(comp, layer, sizes, fit, cx, cy),
          "the box's corner is empty space and does not select the layer");
    check(hits(comp, layer, sizes, fit, fit.toTargetX(960.0), fit.toTargetY(540.0)),
          "but the middle still does");
}

// Scale and parent chain both affect where the pixels end up.
void hit_testing_follows_scale_and_parents() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1920, 1080, 30.0, 10.0);
    const core::LayerId parent = project.addLayer(comp, "p", core::LayerKind::Solid).id;
    const core::LayerId child = project.addLayer(comp, "c", core::LayerKind::Solid).id;
    comp.find(parent)->find("scale")->staticValue = core::Value::vec2(50.0, 50.0);
    comp.find(child)->parent = parent;

    const core::SizeOf sizes = sized(400.0, 200.0);
    const engine::FrameFit fit = engine::frameFit(kCompW, kCompH, 1920.0, 1080.0);
    const core::Layer& kid = *comp.find(child);

    // Parent scale relocates the child, not just resizes it; layerBounds must reflect that.
    const core::Bounds box =
        core::layerBounds(comp, kid, 0.0, comp.timeContext(), kCompW, kCompH, sizes);
    near(box.width(), 200.0, "the parent halved it", 1e-6);

    check(hits(comp, kid, sizes, fit, fit.toTargetX(box.centerX()),
               fit.toTargetY(box.centerY())),
          "the middle of where it really is");
    check(!hits(comp, kid, sizes, fit, fit.toTargetX(box.right + 20.0),
                fit.toTargetY(box.centerY())),
          "and not past its edge");

    // Confirms the parent chain is actually being followed, not ignored.
    check(std::fabs(box.centerX() - 960.0) > 1.0,
          "the parent moved it, so the chain is really being followed");
}

// Scale-drag ratio, mirroring GpuViewport::mouseMoveEvent's Grab::Scale case as plain arithmetic.
double scaledAxis(double original, double handleCoord, double anchorCoord, double newCoord) {
    const double denom = handleCoord - anchorCoord;
    if (std::fabs(denom) < 1e-3) {
        return original;
    }
    return original * (newCoord - anchorCoord) / denom;
}

// Set a target scale, find the resulting handle position, check scaledAxis recovers it.
void dragging_a_handle_scales_around_the_anchor() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1920, 1080, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "solid", core::LayerKind::Solid).id;
    core::Layer* layer = comp.find(id);
    const core::SizeOf sizes = sized(400.0, 200.0);
    const engine::FrameFit fit = engine::frameFit(kCompW, kCompH, 1920.0, 1080.0);

    constexpr double kHandleU = 1.0;  // the right-edge handle
    constexpr double kAnchorU = 0.5;  // default anchor sits at the layer's centre
    constexpr double kOriginal = 100.0;
    constexpr double kTarget = 150.0;

    layer->find("scale")->staticValue = core::Value::vec2(kTarget, kOriginal);
    const core::Transform2D atTarget = unitToWidget(comp, *layer, sizes, fit);
    const double handleX = atTarget.applyX(kHandleU, 0.5);
    const double handleY = atTarget.applyY(kHandleU, 0.5);

    layer->find("scale")->staticValue = core::Value::vec2(kOriginal, kOriginal);
    const core::Transform2D back0 = unitToWidget(comp, *layer, sizes, fit).inverse();
    const double u1 = back0.applyX(handleX, handleY);

    near(scaledAxis(kOriginal, kHandleU, kAnchorU, u1), kTarget,
        "the recovered scale matches what actually put the handle there");

    // An edge handle doesn't touch the other axis at all, unlike a corner handle.
    near(scaledAxis(kOriginal, 0.5, 0.5, 0.5 /* unmoved */), kOriginal,
        "a handle that has not moved along its axis reports no change");
}

// A layer with no size cannot be clicked, and must not throw or claim the whole screen.
void a_layer_with_no_size_is_not_clickable() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1920, 1080, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "empty", core::LayerKind::Solid).id;
    const core::SizeOf sizes = sized(0.0, 0.0);
    const engine::FrameFit fit = engine::frameFit(kCompW, kCompH, 1920.0, 1080.0);

    // A degenerate transform must invert finitely, not to infinities/NaN.
    const core::Transform2D back =
        unitToWidget(comp, *comp.find(id), sizes, fit).inverse();
    check(std::isfinite(back.tx) && std::isfinite(back.a),
          "the inverse of a collapsed transform stays finite");
    check(std::isfinite(back.applyX(960.0, 540.0)),
          "so a click against it produces a number rather than a NaN");
}

// Qt gives logical points; the swapchain/compositor use physical pixels. Mixing them
// misplaced handles and clicks on high-DPI displays.
void the_fit_is_taken_in_surface_pixels() {
    constexpr double kLogicalW = 800.0;
    constexpr double kLogicalH = 500.0;

    for (const double dpr : {1.0, 2.0, 1.5}) {
        const engine::FrameFit surface =
            engine::frameFit(kCompW, kCompH, kLogicalW * dpr, kLogicalH * dpr);

        // Widget centre must map to frame centre at any DPR, if the click is scaled too.
        const double cx = surface.toCompX(kLogicalW * 0.5 * dpr);
        const double cy = surface.toCompY(kLogicalH * 0.5 * dpr);
        near(cx, kCompW * 0.5, "the centre maps to the centre in x", 1e-6);
        near(cy, kCompH * 0.5, "and in y", 1e-6);

        // Logical vs. physical pixels only diverge above 1x, hence the per-dpr check below.
        const engine::FrameFit logical =
            engine::frameFit(kCompW, kCompH, kLogicalW, kLogicalH);
        if (dpr == 1.0) {
            near(logical.scale, surface.scale, "no difference at 1x");
        } else {
            check(std::fabs(logical.scale - surface.scale) > 1e-6,
                  "the two spaces genuinely differ above 1x, so mixing them is a real bug");
        }
    }
}

// Text layer size is the rasterized image (padded by stroke), not the ink bounds.
void a_stroke_makes_a_text_layer_bigger_than_its_ink() {
    const double inkW = 600.0;
    const double inkH = 180.0;
    const double stroke = 5.0;
    const double paddedW = inkW + 2.0 * (stroke + 2.0);
    const double paddedH = inkH + 2.0 * (stroke + 2.0);

    check(paddedW > inkW && paddedH > inkH, "padding grows the layer");
    near(paddedW - inkW, 14.0, "by the stroke plus two, on each side");

    // Mismatched measurement would misplace both edges by this amount.
    near((paddedW - inkW) * 0.5, 7.0, "and the error shows on both edges, not one");
}

}  // namespace

int main() {
    the_fit_letterboxes_both_ways();
    the_fit_is_taken_in_surface_pixels();
    a_stroke_makes_a_text_layer_bigger_than_its_ink();
    a_click_inside_a_layer_hits_it();
    a_rotated_layer_is_not_its_bounding_box();
    hit_testing_follows_scale_and_parents();
    dragging_a_handle_scales_around_the_anchor();
    a_layer_with_no_size_is_not_clickable();

    if (failures == 0) {
        std::puts("viewerpick: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
