// Layer transform and parent chain tests. Cycle cases matter most: an unguarded parent
// loop is an infinite loop in the render path, which just freezes the window.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "ruby/core/Transform.h"

using namespace ruby::core;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void checkNear(double a, double b, const char* what, double eps = 1e-9) {
    if (std::fabs(a - b) > eps) {
        std::fprintf(stderr, "FAIL: %s (got %.9f, want %.9f)\n", what, a, b);
        ++failures;
    }
}

void set(Layer& layer, std::string_view key, double x, double y) {
    if (Property* p = layer.find(key); p != nullptr) {
        p->staticValue = Value::vec2(x, y);
    }
}
// Asserts the transform is finite; the point is mainly that it returns at all.
void checkFinite(const Transform2D& t, const char* what) {
    const bool ok = std::isfinite(t.a) && std::isfinite(t.b) && std::isfinite(t.c) &&
                    std::isfinite(t.d) && std::isfinite(t.tx) && std::isfinite(t.ty);
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (produced a non-finite transform)\n", what);
        ++failures;
    }
}

void set1(Layer& layer, std::string_view key, double v) {
    if (Property* p = layer.find(key); p != nullptr) {
        p->staticValue = Value::scalar(v);
    }
}

// Default layer size, unless a test overrides it.
const SizeOf sizes = [](const Layer&) { return LayerSize{100.0, 100.0}; };

void composition_multiplies_in_the_right_order() {
    // Scale-then-translate != translate-then-scale; order matters.
    const Transform2D scaled = Transform2D::scale(2.0, 2.0);
    const Transform2D moved = Transform2D::translate(10.0, 0.0);

    const Transform2D scaleThenMove = scaled.then(moved);
    checkNear(scaleThenMove.applyX(1.0, 0.0), 12.0, "scale first, then translate");

    const Transform2D moveThenScale = moved.then(scaled);
    checkNear(moveThenScale.applyX(1.0, 0.0), 22.0, "translate first, then scale");
}

void rotation_turns_the_right_way() {
    const Transform2D r = Transform2D::rotate(90.0);
    // Y is down, so positive rotation is clockwise on screen (AE's convention).
    checkNear(r.applyX(1.0, 0.0), 0.0, "90 degrees sends (1,0) off the x axis", 1e-12);
    checkNear(r.applyY(1.0, 0.0), 1.0, "and onto +y, which is downward on screen", 1e-12);

    const Transform2D full = Transform2D::rotate(360.0);
    checkNear(full.applyX(3.0, 4.0), 3.0, "a full turn is the identity in x", 1e-9);
    checkNear(full.applyY(3.0, 4.0), 4.0, "and in y", 1e-9);
}

// Anchor point is the pivot for rotation/scale.
void the_anchor_point_is_the_pivot() {
    Project project;
    Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const LayerId id = project.addLayer(comp, "l", LayerKind::Solid).id;
    Layer* layer = comp.find(id);

    set(*layer, "position", 50.0, 50.0);
    set(*layer, "anchor_point", 0.0, 0.0);
    set1(*layer, "rotation", 90.0);

    const TimeContext ctx = comp.timeContext();
    const Transform2D centred = layerTransform(*layer, 0.0, ctx, 1000, 1000, LayerSize{200, 100});

    // With a centred anchor, the layer's own origin lands exactly on Position.
    checkNear(centred.applyX(0.0, 0.0), 500.0, "origin lands on position in x");
    checkNear(centred.applyY(0.0, 0.0), 500.0, "and in y");

    // Move the anchor off centre and the same point swings away from Position.
    set(*layer, "anchor_point", 50.0, 0.0);
    const Transform2D offset = layerTransform(*layer, 0.0, ctx, 1000, 1000, LayerSize{200, 100});
    const double dx = offset.applyX(0.0, 0.0) - 500.0;
    const double dy = offset.applyY(0.0, 0.0) - 500.0;
    check(std::fabs(dx) > 1.0 || std::fabs(dy) > 1.0,
          "an off-centre anchor makes the layer pivot around a different point");
}

void a_parent_moves_its_child() {
    Project project;
    Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);

    const LayerId parentId = project.addLayer(comp, "null", LayerKind::Null).id;
    const LayerId childId = project.addLayer(comp, "child", LayerKind::Solid).id;

    set(*comp.find(parentId), "position", 50.0, 50.0);
    set(*comp.find(childId), "position", 50.0, 50.0);
    set(*comp.find(childId), "anchor_point", 0.0, 0.0);
    set(*comp.find(parentId), "anchor_point", 0.0, 0.0);

    const TimeContext ctx = comp.timeContext();
    const Layer& child = *comp.find(childId);

    const Transform2D alone =
        resolvedTransform(comp, child, 0.0, ctx, 1000, 1000, sizes);

    comp.find(childId)->parent = parentId;
    const Transform2D parented =
        resolvedTransform(comp, *comp.find(childId), 0.0, ctx, 1000, 1000, sizes);

    check(std::fabs(parented.applyX(0.0, 0.0) - alone.applyX(0.0, 0.0)) > 1.0,
          "parenting to a layer at the centre displaces the child");

    // A parent at the origin should leave the child where it was.
    set(*comp.find(parentId), "position", 0.0, 0.0);
    const Transform2D neutral =
        resolvedTransform(comp, *comp.find(childId), 0.0, ctx, 1000, 1000, sizes);
    checkNear(neutral.applyX(0.0, 0.0), alone.applyX(0.0, 0.0),
              "a parent at the origin leaves the child alone in x", 1e-9);
}

// Cycle cases unreachable through the UI but each an infinite loop waiting to happen.
void parent_cycles_terminate() {
    Project project;
    Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);

    const LayerId a = project.addLayer(comp, "a", LayerKind::Null).id;
    const LayerId b = project.addLayer(comp, "b", LayerKind::Null).id;
    const LayerId c = project.addLayer(comp, "c", LayerKind::Null).id;

    // A layer parented to itself.
    comp.find(a)->parent = a;
    check(hasParentCycle(comp, a), "a self-parent is a cycle");
    const TimeContext ctx = comp.timeContext();
    checkFinite(resolvedTransform(comp, *comp.find(a), 0.0, ctx, 1000, 1000, sizes),
                "resolving a self-parent returns instead of spinning");

    // A two-layer loop.
    comp.find(a)->parent = b;
    comp.find(b)->parent = a;
    check(hasParentCycle(comp, a), "a two layer loop is a cycle");
    check(hasParentCycle(comp, b), "from either end");
    checkFinite(resolvedTransform(comp, *comp.find(a), 0.0, ctx, 1000, 1000, sizes),
                "a two layer loop terminates");

    // A three-layer loop.
    comp.find(a)->parent = b;
    comp.find(b)->parent = c;
    comp.find(c)->parent = a;
    check(hasParentCycle(comp, a), "a three layer loop is a cycle");
    checkFinite(resolvedTransform(comp, *comp.find(b), 0.0, ctx, 1000, 1000, sizes),
                "a three layer loop terminates");

    // A dangling parent id, as a hand-edited file could produce.
    comp.find(a)->parent = 9999;
    comp.find(b)->parent.reset();
    comp.find(c)->parent.reset();
    check(!hasParentCycle(comp, a), "a dangling parent is not a cycle");
    checkFinite(resolvedTransform(comp, *comp.find(a), 0.0, ctx, 1000, 1000, sizes),
                "a dangling parent renders unparented rather than failing");
}

// Bad parenting must be refused before it's stored, not caught later.
void bad_parenting_is_refused_up_front() {
    Project project;
    Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);

    const LayerId a = project.addLayer(comp, "a", LayerKind::Null).id;
    const LayerId b = project.addLayer(comp, "b", LayerKind::Null).id;
    const LayerId c = project.addLayer(comp, "c", LayerKind::Null).id;

    check(!canParentTo(comp, a, a), "a layer cannot parent to itself");
    check(canParentTo(comp, a, b), "unrelated layers can be parented");

    comp.find(b)->parent = a;  // b is now below a
    check(!canParentTo(comp, a, b), "a cannot parent to its own child");
    check(canParentTo(comp, c, b), "but an unrelated layer still can");

    comp.find(c)->parent = b;  // chain is now c -> b -> a
    check(!canParentTo(comp, a, c), "nor to a grandchild");
}

void a_long_chain_is_bounded() {
    Project project;
    Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);

    // No cycle, just deeper than the cap; must terminate on depth alone.
    LayerId previous = 0;
    LayerId first = 0;
    for (int i = 0; i < kMaxParentDepth + 20; ++i) {
        const LayerId id = project.addLayer(comp, "n", LayerKind::Null).id;
        if (previous != 0) {
            comp.find(previous)->parent = id;
        } else {
            first = id;
        }
        previous = id;
    }
    const TimeContext ctx = comp.timeContext();
    checkFinite(resolvedTransform(comp, *comp.find(first), 0.0, ctx, 1000, 1000, sizes),
                "a chain longer than the cap resolves without spinning");
}

// canParentTo drives the Parent menu's greyed-out state; a false negative here creates a loop.
void the_parent_menu_refuses_exactly_the_loops() {
    Project project;
    Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);

    const LayerId a = project.addLayer(comp, "a", LayerKind::Null).id;
    const LayerId b = project.addLayer(comp, "b", LayerKind::Null).id;
    const LayerId c = project.addLayer(comp, "c", LayerKind::Null).id;
    const LayerId d = project.addLayer(comp, "d", LayerKind::Null).id;

    // Chain: c -> b -> a. Everything below a is forbidden as a parent OF a.
    comp.find(b)->parent = a;
    comp.find(c)->parent = b;

    check(!canParentTo(comp, a, a), "self");
    check(!canParentTo(comp, a, b), "child");
    check(!canParentTo(comp, a, c), "grandchild");
    check(canParentTo(comp, a, d), "an unrelated layer is fine");

    // Parenting down the chain (deeper to shallower) is always fine.
    check(canParentTo(comp, d, c), "parenting down the existing chain is fine");

    comp.find(d)->parent = c;
    check(!hasParentCycle(comp, d), "an accepted parenting does not create a cycle");
    check(!hasParentCycle(comp, a), "nor anywhere else in the chain");
}

// A NaN property must not produce a NaN matrix; that reaches the GPU as an undiagnosable
// vanished layer.
void non_finite_values_do_not_poison_the_matrix() {
    Project project;
    Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const LayerId id = project.addLayer(comp, "l", LayerKind::Solid).id;
    const TimeContext ctx = comp.timeContext();

    const auto finite = [](const Transform2D& t) {
        return std::isfinite(t.a) && std::isfinite(t.b) && std::isfinite(t.c) &&
               std::isfinite(t.d) && std::isfinite(t.tx) && std::isfinite(t.ty);
    };

    set1(*comp.find(id), "rotation", std::nan(""));
    check(finite(layerTransform(*comp.find(id), 0.0, ctx, 1000, 1000, LayerSize{100, 100})),
          "a NaN rotation falls back instead of producing a NaN matrix");

    set1(*comp.find(id), "rotation", 0.0);
    set(*comp.find(id), "position", std::nan(""), 50.0);
    check(finite(layerTransform(*comp.find(id), 0.0, ctx, 1000, 1000, LayerSize{100, 100})),
          "and so does a NaN position");

    set(*comp.find(id), "position", std::numeric_limits<double>::infinity(), 50.0);
    check(finite(layerTransform(*comp.find(id), 0.0, ctx, 1000, 1000, LayerSize{100, 100})),
          "and an infinite one");

    set(*comp.find(id), "position", 50.0, 50.0);
    set(*comp.find(id), "scale", std::nan(""), 100.0);
    check(finite(layerTransform(*comp.find(id), 0.0, ctx, 1000, 1000, LayerSize{100, 100})),
          "and a NaN scale");
}

// Zero/negative scale are legitimate (hide, flip) and must not be guarded away.
void degenerate_but_legal_scales_are_left_alone() {
    Project project;
    Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const LayerId id = project.addLayer(comp, "l", LayerKind::Solid).id;
    const TimeContext ctx = comp.timeContext();

    set(*comp.find(id), "scale", 0.0, 0.0);
    const Transform2D collapsed =
        layerTransform(*comp.find(id), 0.0, ctx, 1000, 1000, LayerSize{100, 100});
    checkNear(collapsed.a * collapsed.d - collapsed.b * collapsed.c, 0.0,
              "a zero scale collapses the layer, which is what it should do");

    set(*comp.find(id), "scale", -100.0, 100.0);
    const Transform2D flipped =
        layerTransform(*comp.find(id), 0.0, ctx, 1000, 1000, LayerSize{100, 100});
    check(flipped.a * flipped.d - flipped.b * flipped.c < 0.0,
          "a negative scale mirrors rather than being clamped away");
}

}  // namespace

int main() {
    composition_multiplies_in_the_right_order();
    rotation_turns_the_right_way();
    the_anchor_point_is_the_pivot();
    a_parent_moves_its_child();
    parent_cycles_terminate();
    bad_parenting_is_refused_up_front();
    a_long_chain_is_bounded();
    the_parent_menu_refuses_exactly_the_loops();
    non_finite_values_do_not_poison_the_matrix();
    degenerate_but_legal_scales_are_left_alone();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("transform: all checks passed");
    return EXIT_SUCCESS;
}
