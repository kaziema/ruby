// End-to-end: expressions changing what the compositor draws, via core::evaluate/layerTransform.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ruby/core/Expressions.h"
#include "ruby/core/Transform.h"
#include "ruby/script/LuaHost.h"

using namespace ruby;

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

const core::SizeOf sizes = [](const core::Layer&) {
    return core::LayerSize{100.0, 100.0};
};

core::Property* prop(core::Layer& layer, std::string_view key) { return layer.find(key); }

// With no host installed, a project with expressions must still read.
void without_a_host_expressions_are_ignored() {
    check(core::expressionHost() == nullptr, "no host by default");

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;
    prop(*comp.find(id), "rotation")->expression = "999";

    const core::Value v =
        core::evaluate(*comp.find(id), *prop(*comp.find(id), "rotation"), 0.0,
                       comp.timeContext());
    checkNear(v.c[0], 0.0, "the keyframed value is used, not the expression");
}

void an_expression_drives_a_property() {
    auto host = script::LuaHost::create();
    check(host != nullptr, "an interpreter starts");
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;

    prop(*comp.find(id), "rotation")->expression = "time * 90";
    const core::TimeContext ctx = comp.timeContext();

    checkNear(core::evaluate(*comp.find(id), *prop(*comp.find(id), "rotation"), 0.0, ctx).c[0],
              0.0, "at zero seconds");
    checkNear(core::evaluate(*comp.find(id), *prop(*comp.find(id), "rotation"), 2.0, ctx).c[0],
              180.0, "at two seconds");

    // And it reaches the transform the compositor actually uses.
    const core::Transform2D turned =
        core::layerTransform(*comp.find(id), 1.0, ctx, 1000, 1000, core::LayerSize{100, 100});
    checkNear(turned.a, std::cos(90.0 * 3.14159265358979323846 / 180.0),
              "the transform is genuinely rotated by the expression", 1e-9);
}

// `value` has to be the property's own keyframed value, or `value + 20` means nothing.
void value_is_the_property_s_own_value() {
    auto host = script::LuaHost::create();
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;

    core::Property* position = prop(*comp.find(id), "position");
    position->staticValue = core::Value::vec2(50.0, 40.0);
    position->expression = "value + vec(0, 10)";

    const core::Value v =
        core::evaluate(*comp.find(id), *position, 0.0, comp.timeContext());
    check(v.count == 2, "a vec2 comes back");
    checkNear(v.c[0], 50.0, "x untouched");
    checkNear(v.c[1], 50.0, "y offset by the expression");
}

// A wrong-shaped expression result must not change the property's shape.
void a_wrong_shaped_result_falls_back() {
    auto host = script::LuaHost::create();
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;

    core::Property* position = prop(*comp.find(id), "position");
    position->staticValue = core::Value::vec2(50.0, 40.0);

    // A scalar broadcast across a vector is the one widening worth doing.
    position->expression = "10";
    const core::Value broadcast =
        core::evaluate(*comp.find(id), *position, 0.0, comp.timeContext());
    check(broadcast.count == 2, "a scalar result keeps the property's shape");
    checkNear(broadcast.c[1], 10.0, "broadcast across components");

    // A vec3 on a vec2 property is not a widening, it is a mistake.
    position->expression = "vec(1, 2, 3)";
    const core::Value refused =
        core::evaluate(*comp.find(id), *position, 0.0, comp.timeContext());
    check(refused.count == 2, "a wider result does not change the property's shape");
    checkNear(refused.c[0], 50.0, "and the keyframed value is used instead");
}

// A broken expression must not stop a frame drawing. It falls back and is recorded.
void a_broken_expression_falls_back_and_is_reported() {
    auto host = script::LuaHost::create();
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;

    core::Property* rotation = prop(*comp.find(id), "rotation");
    rotation->staticValue = core::Value::scalar(45.0);
    rotation->expression = "this is not lua";

    const core::Value v =
        core::evaluate(*comp.find(id), *rotation, 0.0, comp.timeContext());
    checkNear(v.c[0], 45.0, "the keyframed value is used when the expression fails");
    check(host->failures().size() == 1, "and the failure is recorded once");

    // Sixty frames of the same broken expression is one problem, not sixty.
    for (int frame = 0; frame < 60; ++frame) {
        const core::Value ignored =
            core::evaluate(*comp.find(id), *rotation, frame / 30.0, comp.timeContext());
        (void)ignored;
    }
    check(host->failures().size() == 1, "not once per frame");

    // An infinite loop in one property must not take the render with it.
    rotation->expression = "while true do end";
    const core::Value spun =
        core::evaluate(*comp.find(id), *rotation, 0.0, comp.timeContext());
    checkNear(spun.c[0], 45.0, "a runaway expression falls back like any other failure");
}

// Seed stability is what makes renders reproducible across sessions.
void seeds_are_stable_and_well_separated() {
    const std::uint64_t a = core::expressionSeed(1, "position");
    check(a == core::expressionSeed(1, "position"), "the same layer and key give the same seed");
    check(a != core::expressionSeed(2, "position"), "a different layer differs");
    check(a != core::expressionSeed(1, "scale"), "a different property differs");

    // Consecutive layer ids are the common case and must not produce related seeds.
    check(core::expressionSeed(1, "position") != core::expressionSeed(2, "position"),
          "and consecutive ids are not related");
}

void two_layers_with_the_same_wiggle_move_differently() {
    auto host = script::LuaHost::create();
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const core::LayerId a = project.addLayer(comp, "a", core::LayerKind::Solid).id;
    const core::LayerId b = project.addLayer(comp, "b", core::LayerKind::Solid).id;

    for (const core::LayerId id : {a, b}) {
        core::Property* rotation = prop(*comp.find(id), "rotation");
        rotation->staticValue = core::Value::scalar(0.0);
        rotation->expression = "wiggle(5, 30)";
    }
    const core::TimeContext ctx = comp.timeContext();
    const double first =
        core::evaluate(*comp.find(a), *prop(*comp.find(a), "rotation"), 1.0, ctx).c[0];
    const double second =
        core::evaluate(*comp.find(b), *prop(*comp.find(b), "rotation"), 1.0, ctx).c[0];

    check(std::fabs(first - second) > 1e-9,
          "identical expressions on different layers do not move in sympathy");
    checkNear(core::evaluate(*comp.find(a), *prop(*comp.find(a), "rotation"), 1.0, ctx).c[0],
              first, "and each layer is repeatable");
}

// loopOut() with two keyframes is the common looping-motion pattern.
void loop_out_repeats_the_animation() {
    auto host = script::LuaHost::create();
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 20.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;
    const core::TimeContext ctx = comp.timeContext();

    core::Property* rotation = prop(*comp.find(id), "rotation");
    rotation->addKey({core::TimeValue::seconds(0.0), core::Value::scalar(0.0),
                      core::Interpolation::Linear, 0.0, 0.0, 0.0}, ctx);
    rotation->addKey({core::TimeValue::seconds(2.0), core::Value::scalar(100.0),
                      core::Interpolation::Linear, 0.0, 0.0, 0.0}, ctx);

    const auto at = [&](double t) {
        return core::evaluate(*comp.find(id), *prop(*comp.find(id), "rotation"), t, ctx).c[0];
    };

    rotation->expression = "loopOut()";

    // Inside the keyframed range the keyframes speak for themselves.
    checkNear(at(1.0), 50.0, "inside the range the animation is untouched");
    checkNear(at(2.0), 100.0, "and at the last keyframe");

    // Past the end it repeats from the start.
    checkNear(at(3.0), 50.0, "one second past the end matches one second in");
    checkNear(at(5.0), 50.0, "and again a cycle later");

    // pingpong runs the second pass backwards.
    rotation->expression = "loopOut('pingpong')";
    checkNear(at(3.0), 50.0, "pingpong's first pass back is a mirror");
    checkNear(at(2.5), 75.0, "reflecting rather than restarting");

    // offset keeps travelling instead of snapping back.
    rotation->expression = "loopOut('offset')";
    checkNear(at(3.0), 150.0, "offset carries the accumulated distance");
    checkNear(at(5.0), 250.0, "and keeps accumulating");
}

void loop_out_is_safe_on_an_unfinished_animation() {
    auto host = script::LuaHost::create();
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 20.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;
    const core::TimeContext ctx = comp.timeContext();

    core::Property* rotation = prop(*comp.find(id), "rotation");
    rotation->staticValue = core::Value::scalar(30.0);
    rotation->expression = "loopOut()";

    // loopOut must not error with zero/one keyframes (mid-animation is when it gets typed).
    checkNear(core::evaluate(*comp.find(id), *rotation, 5.0, ctx).c[0], 30.0,
              "with no keyframes it returns the value unchanged");

    rotation->addKey({core::TimeValue::seconds(0.0), core::Value::scalar(10.0),
                      core::Interpolation::Linear, 0.0, 0.0, 0.0}, ctx);
    checkNear(core::evaluate(*comp.find(id), *rotation, 5.0, ctx).c[0], 10.0,
              "and with one keyframe it still cannot loop, so it does not");
}

// Regression: Opacity bypassed core::evaluate and silently ignored its expression.
void every_transform_property_is_expressible() {
    auto host = script::LuaHost::create();
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;
    const core::TimeContext ctx = comp.timeContext();

    for (const char* key : {"position", "scale", "rotation", "anchor_point", "opacity"}) {
        core::Property* target = prop(*comp.find(id), key);
        check(target != nullptr, "the property exists");
        if (target == nullptr) {
            continue;
        }
        const core::Value plain = core::evaluate(*comp.find(id), *target, 0.0, ctx);

        // Scalar broadcast keeps the property's shape, works for vectors and scalars alike.
        target->expression = "7";
        const core::Value driven = core::evaluate(*comp.find(id), *target, 0.0, ctx);
        check(driven.count == plain.count, "the shape is preserved");
        check(driven.c[0] > 6.9 && driven.c[0] < 7.1,
              "and the expression actually drives it");
        target->expression.reset();
    }
}

// A NaN from an expression must never reach the transform matrix.
void a_non_finite_expression_cannot_reach_the_transform() {
    auto host = script::LuaHost::create();
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;
    const core::TimeContext ctx = comp.timeContext();

    core::Property* rotation = prop(*comp.find(id), "rotation");
    rotation->staticValue = core::Value::scalar(30.0);
    rotation->expression = "0/0";

    checkNear(core::evaluate(*comp.find(id), *rotation, 0.0, ctx).c[0], 30.0,
              "a NaN expression falls back to the keyframed value");

    const core::Transform2D t =
        core::layerTransform(*comp.find(id), 0.0, ctx, 1000, 1000, core::LayerSize{100, 100});
    check(std::isfinite(t.a) && std::isfinite(t.tx),
          "and the transform it feeds stays finite");
}

}  // namespace

int main() {
    without_a_host_expressions_are_ignored();
    an_expression_drives_a_property();
    value_is_the_property_s_own_value();
    a_wrong_shaped_result_falls_back();
    a_broken_expression_falls_back_and_is_reported();
    seeds_are_stable_and_well_separated();
    two_layers_with_the_same_wiggle_move_differently();
    loop_out_repeats_the_animation();
    loop_out_is_safe_on_an_unfinished_animation();
    every_transform_property_is_expressible();
    a_non_finite_expression_cannot_reach_the_transform();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("expressions: all checks passed");
    return EXIT_SUCCESS;
}
