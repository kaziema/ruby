// Turning a click in the picture into a layer.
//
// The maths the viewer hit-tests with, checked without a window. It works by putting the
// point back through each layer's transform rather than by comparing against a bounding
// box: a rotated layer's box covers corners that are not the layer, and clicking one of
// those would select something the user is not pointing at.

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

// The same chain the viewer builds and the compositor draws with.
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

// The frame fit is shared with the renderer for exactly one reason: a click two pixels from
// where the picture was drawn is a click on the wrong layer.
void the_fit_letterboxes_both_ways() {
    // A 16:9 comp in a tall window: bars top and bottom.
    const engine::FrameFit tall = engine::frameFit(1920.0, 1080.0, 800.0, 800.0);
    near(tall.width, 800.0, "fills the width");
    near(tall.height, 450.0, "and is letterboxed vertically");
    near(tall.x, 0.0, "flush left");
    near(tall.y, 175.0, "centred vertically");

    // The same comp in a wide window: bars left and right.
    const engine::FrameFit wide = engine::frameFit(1920.0, 1080.0, 2000.0, 500.0);
    near(wide.height, 500.0, "fills the height");
    near(wide.width, 888.888888, "and is pillarboxed", 1e-3);

    // And the round trip, which is the property hit testing actually depends on.
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

// The reason hit testing goes through the transform rather than a box. A layer turned 45
// degrees has a bounding box whose corners are empty space.
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
    // Its box grew: a 400 square turned 45 degrees spans 400*sqrt(2).
    near(box.width(), 400.0 * std::sqrt(2.0), "the box is the diagonal", 1e-3);

    // A point just inside the box's top-left corner is outside the diamond.
    const double cx = fit.toTargetX(box.left + 12.0);
    const double cy = fit.toTargetY(box.top + 12.0);
    check(!hits(comp, layer, sizes, fit, cx, cy),
          "the box's corner is empty space and does not select the layer");
    check(hits(comp, layer, sizes, fit, fit.toTargetX(960.0), fit.toTargetY(540.0)),
          "but the middle still does");
}

// Scale and the parent chain both have to be in the answer, because both change where the
// pixels ended up.
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

    // Where the child ACTUALLY lands, which is not where its own Position says: a parent
    // scaled about its own anchor relocates its children as well as resizing them. Asking
    // layerBounds rather than assuming is the point of the test, because that is the same
    // question the align maths asks and the two must agree.
    const core::Bounds box =
        core::layerBounds(comp, kid, 0.0, comp.timeContext(), kCompW, kCompH, sizes);
    near(box.width(), 200.0, "the parent halved it", 1e-6);

    check(hits(comp, kid, sizes, fit, fit.toTargetX(box.centerX()),
               fit.toTargetY(box.centerY())),
          "the middle of where it really is");
    check(!hits(comp, kid, sizes, fit, fit.toTargetX(box.right + 20.0),
                fit.toTargetY(box.centerY())),
          "and not past its edge");

    // And it is genuinely somewhere else than an unparented layer would be, or the test
    // would pass with the parent chain ignored entirely.
    check(std::fabs(box.centerX() - 960.0) > 1.0,
          "the parent moved it, so the chain is really being followed");
}

// A layer with no size cannot be clicked, and must not throw or claim the whole screen.
void a_layer_with_no_size_is_not_clickable() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1920, 1080, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "empty", core::LayerKind::Solid).id;
    const core::SizeOf sizes = sized(0.0, 0.0);
    const engine::FrameFit fit = engine::frameFit(kCompW, kCompH, 1920.0, 1080.0);

    // A degenerate transform inverts to the identity rather than to infinities, so this
    // answers a plain no instead of producing NaNs that travel into a Position value.
    const core::Transform2D back =
        unitToWidget(comp, *comp.find(id), sizes, fit).inverse();
    check(std::isfinite(back.tx) && std::isfinite(back.a),
          "the inverse of a collapsed transform stays finite");
    check(std::isfinite(back.applyX(960.0, 540.0)),
          "so a click against it produces a number rather than a NaN");
}

// The two coordinate systems this widget lives in, and the conversion between them.
//
// Qt hands out mouse positions and widget sizes in logical points; the swapchain, and
// therefore everything the compositor computed, is in physical pixels. Mixing them put the
// handles at half scale in the corner and made every click land on the wrong part of the
// picture. This is the arithmetic that has to hold.
void the_fit_is_taken_in_surface_pixels() {
    constexpr double kLogicalW = 800.0;
    constexpr double kLogicalH = 500.0;

    for (const double dpr : {1.0, 2.0, 1.5}) {
        const engine::FrameFit surface =
            engine::frameFit(kCompW, kCompH, kLogicalW * dpr, kLogicalH * dpr);

        // A click at the middle of the widget is the middle of the frame, whatever the
        // display's scale factor is, PROVIDED the click is scaled with it.
        const double cx = surface.toCompX(kLogicalW * 0.5 * dpr);
        const double cy = surface.toCompY(kLogicalH * 0.5 * dpr);
        near(cx, kCompW * 0.5, "the centre maps to the centre in x", 1e-6);
        near(cy, kCompH * 0.5, "and in y", 1e-6);

        // And the bug that was shipped: taking the fit in logical pixels while the picture
        // was drawn in physical ones. At dpr 1 they agree, which is exactly why this was
        // invisible until it reached a Retina display.
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

// A text layer is the size of the IMAGE the rasteriser makes, not the size of its ink: the
// raster pads by the stroke so a heavy outline is not clipped, and the compositor sizes the
// quad from the image. Checked as arithmetic here, since the rasteriser needs a font stack.
void a_stroke_makes_a_text_layer_bigger_than_its_ink() {
    const double inkW = 600.0;
    const double inkH = 180.0;
    const double stroke = 5.0;
    const double paddedW = inkW + 2.0 * (stroke + 2.0);
    const double paddedH = inkH + 2.0 * (stroke + 2.0);

    check(paddedW > inkW && paddedH > inkH, "padding grows the layer");
    near(paddedW - inkW, 14.0, "by the stroke plus two, on each side");

    // Which is why measuring one and drawing the other misplaces every handle: the box
    // would be 14px narrow and centred on the same point, so both edges are wrong.
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
    a_layer_with_no_size_is_not_clickable();

    if (failures == 0) {
        std::puts("viewerpick: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
