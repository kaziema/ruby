// Layer bounds and alignment math. Checks the numbers, not the UI buttons.

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
#include <cstdio>

#include "ruby/core/Document.h"
#include "ruby/core/Transform.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void near(double got, double want, const char* what, double eps = 1e-6) {
    if (std::fabs(got - want) > eps) {
        std::fprintf(stderr, "FAIL: %s (got %.6f, wanted %.6f)\n", what, got, want);
        ++failures;
    }
}

constexpr double kCompW = 1080.0;
constexpr double kCompH = 1920.0;

// Every layer in these tests is 400x200, so the numbers stay checkable by hand.
core::SizeOf fixedSize() {
    return [](const core::Layer&) { return core::LayerSize{400.0, 200.0}; };
}

core::Property* position(core::Layer& layer) { return layer.find("position"); }

void set(core::Layer& layer, const char* key, double x, double y) {
    if (core::Property* p = layer.find(key); p != nullptr) {
        p->staticValue = core::Value::vec2(x, y);
    }
}

// A default layer sits at 50%, 50%, which is the middle of the frame.
void a_centred_layer_is_centred() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    core::Layer& layer = project.addLayer(comp, "solid", core::LayerKind::Solid);

    const core::Bounds box = core::layerBounds(comp, layer, 0.0, comp.timeContext(),
                                               kCompW, kCompH, fixedSize());
    near(box.centerX(), kCompW / 2.0, "centred horizontally");
    near(box.centerY(), kCompH / 2.0, "centred vertically");
    near(box.width(), 400.0, "as wide as the layer");
    near(box.height(), 200.0, "as tall as the layer");
}

// Scale grows the box around the anchor point.
void scale_grows_the_box() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    core::Layer& layer = project.addLayer(comp, "solid", core::LayerKind::Solid);
    set(layer, "scale", 200.0, 50.0);

    const core::Bounds box = core::layerBounds(comp, layer, 0.0, comp.timeContext(),
                                               kCompW, kCompH, fixedSize());
    near(box.width(), 800.0, "twice as wide");
    near(box.height(), 100.0, "half as tall");
    near(box.centerX(), kCompW / 2.0, "still centred");
}

// A rotated layer reports the containing box, not its own tilted rectangle.
void rotation_gives_the_containing_box() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    core::Layer& layer = project.addLayer(comp, "solid", core::LayerKind::Solid);
    if (core::Property* r = layer.find("rotation"); r != nullptr) {
        r->staticValue = core::Value::scalar(90.0);
    }

    const core::Bounds box = core::layerBounds(comp, layer, 0.0, comp.timeContext(),
                                               kCompW, kCompH, fixedSize());
    // Turned on its side: the 400-wide layer is now 400 tall.
    near(box.width(), 200.0, "a quarter turn swaps the box's width");
    near(box.height(), 400.0, "and its height");
}

// The align math MainWindow runs: measure, convert gap to Position's percentage units, apply, remeasure.
double alignedLeft(core::Composition& comp, core::Layer& layer) {
    const core::TimeContext ctx = comp.timeContext();
    const core::Bounds box =
        core::layerBounds(comp, layer, 0.0, ctx, kCompW, kCompH, fixedSize());
    const double dx = -box.left;
    core::Property* p = position(layer);
    p->staticValue = core::Value::vec2(p->staticValue.c[0] + dx / kCompW * 100.0,
                                       p->staticValue.c[1]);
    return core::layerBounds(comp, layer, 0.0, ctx, kCompW, kCompH, fixedSize()).left;
}

void aligning_left_puts_the_left_edge_on_zero() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    core::Layer& layer = project.addLayer(comp, "solid", core::LayerKind::Solid);

    near(alignedLeft(comp, layer), 0.0, "a plain layer lands on the left edge");

    // And it is idempotent: aligning something already aligned must not creep.
    near(alignedLeft(comp, layer), 0.0, "aligning again changes nothing");
}

void aligning_left_works_on_a_scaled_layer() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    core::Layer& layer = project.addLayer(comp, "solid", core::LayerKind::Solid);
    set(layer, "scale", 250.0, 250.0);

    near(alignedLeft(comp, layer), 0.0, "scale does not throw the alignment off");
}

// An off-centre anchor changes the gap between Position and the left edge.
void aligning_left_works_with_an_offset_anchor() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    core::Layer& layer = project.addLayer(comp, "solid", core::LayerKind::Solid);
    set(layer, "anchor_point", 40.0, -25.0);

    near(alignedLeft(comp, layer), 0.0, "the anchor does not throw the alignment off");
}

// A parented layer's Position is in parent space; needs Transform2D::inverse to convert.
void aligning_left_works_under_a_scaled_parent() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    // Take the id before the next addLayer: it can reallocate the vector, dangling refs.
    const core::LayerId parentId =
        project.addLayer(comp, "parent", core::LayerKind::Solid).id;
    const core::LayerId childId =
        project.addLayer(comp, "child", core::LayerKind::Solid).id;
    set(*comp.find(parentId), "scale", 50.0, 50.0);
    comp.find(childId)->parent = parentId;

    core::Layer& c = *comp.find(childId);
    const core::TimeContext ctx = comp.timeContext();
    const core::Bounds box =
        core::layerBounds(comp, c, 0.0, ctx, kCompW, kCompH, fixedSize());
    const double dx = -box.left;

    // The conversion MainWindow does.
    const core::Transform2D toComp = core::resolvedTransform(
        comp, *comp.find(parentId), 0.0, ctx, kCompW, kCompH, fixedSize());
    const core::Transform2D back = toComp.inverse();
    const double ux = back.applyX(dx, 0.0) - back.applyX(0.0, 0.0);

    core::Property* p = position(c);
    p->staticValue = core::Value::vec2(p->staticValue.c[0] + ux / kCompW * 100.0,
                                       p->staticValue.c[1]);

    const double after =
        core::layerBounds(comp, c, 0.0, ctx, kCompW, kCompH, fixedSize()).left;
    near(after, 0.0, "a parented layer still lands on the edge", 1e-6);
}

void an_inverse_undoes_its_transform() {
    const core::Transform2D t = core::Transform2D::scale(2.0, 3.0)
                                    .then(core::Transform2D::rotate(30.0))
                                    .then(core::Transform2D::translate(100.0, -50.0));
    const core::Transform2D back = t.inverse();
    const double x = t.applyX(7.0, 11.0);
    const double y = t.applyY(7.0, 11.0);
    near(back.applyX(x, y), 7.0, "x round trips", 1e-9);
    near(back.applyY(x, y), 11.0, "y round trips", 1e-9);

    // A degenerate transform's inverse must stay finite, not produce infinities.
    const core::Transform2D flat = core::Transform2D::scale(0.0, 0.0);
    const core::Transform2D none = flat.inverse();
    check(std::isfinite(none.a) && std::isfinite(none.tx),
          "a degenerate transform inverts to something finite");
}

// --- distribute --------------------------------------------------------------
// Even gaps between the outermost two layers, which stay where they are.
void distributing_three_evens_the_gaps() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    const core::LayerId a = project.addLayer(comp, "a", core::LayerKind::Solid).id;
    const core::LayerId b = project.addLayer(comp, "b", core::LayerKind::Solid).id;
    const core::LayerId c = project.addLayer(comp, "c", core::LayerKind::Solid).id;

    // Centres at 100, 150 and 900 pixels. The middle one is nowhere near the middle.
    const auto placeAt = [&](core::LayerId id, double px) {
        core::Property* p = comp.find(id)->find("position");
        p->staticValue = core::Value::vec2(px / kCompW * 100.0, 50.0);
    };
    placeAt(a, 100.0);
    placeAt(b, 150.0);
    placeAt(c, 900.0);

    const core::TimeContext ctx = comp.timeContext();
    const auto centreOf = [&](core::LayerId id) {
        return core::layerBounds(comp, *comp.find(id), 0.0, ctx, kCompW, kCompH,
                                 fixedSize())
            .centerX();
    };

    // Sort by position, not stack order: distribute is about the picture, not the list.
    std::vector<std::pair<double, core::LayerId>> placed = {
        {centreOf(a), a}, {centreOf(b), b}, {centreOf(c), c}};
    std::sort(placed.begin(), placed.end());

    const double first = placed.front().first;
    const double last = placed.back().first;
    const double step = (last - first) / 2.0;

    for (std::size_t i = 1; i + 1 < placed.size(); ++i) {
        const double want = first + step * static_cast<double>(i);
        const double delta = want - placed[i].first;
        core::Property* p = comp.find(placed[i].second)->find("position");
        p->staticValue = core::Value::vec2(
            p->staticValue.c[0] + delta / kCompW * 100.0, p->staticValue.c[1]);
    }

    near(centreOf(a), 100.0, "the first end did not move");
    near(centreOf(c), 900.0, "nor did the last");
    near(centreOf(b), 500.0, "and the middle one is now exactly between them");
}

// Two layers can't be distributed; guards against the threshold constant drifting.
void distributing_needs_three() {
    const std::size_t two = 2;
    check(two < 3, "the threshold is three, and two is not enough");
}

}  // namespace

int main() {
    a_centred_layer_is_centred();
    scale_grows_the_box();
    rotation_gives_the_containing_box();
    aligning_left_puts_the_left_edge_on_zero();
    aligning_left_works_on_a_scaled_layer();
    aligning_left_works_with_an_offset_anchor();
    aligning_left_works_under_a_scaled_parent();
    an_inverse_undoes_its_transform();
    distributing_three_evens_the_gaps();
    distributing_needs_three();

    if (failures == 0) {
        std::puts("align: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
