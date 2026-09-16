// Tests for keyframes, easing, and property evaluation.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ruby/core/Animation.h"

using namespace ruby::core;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void checkNear(double a, double b, const char* what, double eps = 1e-6) {
    if (std::fabs(a - b) > eps) {
        std::fprintf(stderr, "FAIL: %s (got %.8f, want %.8f)\n", what, a, b);
        ++failures;
    }
}

const TimeContext kAt30{30.0, 120.0, false};

// Any easing must still start/end exactly at 0/1; downstream code assumes this.
void easing_pins_both_endpoints() {
    const double eases[][3] = {{0.0, 0.0, 0.0}, {0.68, 1.0, 0.0}, {0.5, 0.5, 0.12},
                              {1.0, 1.0, 0.4}, {0.0, 1.0, 0.0}};
    for (const auto& e : eases) {
        checkNear(easeCurve(0.0, e[0], e[1], e[2]), 0.0, "ease starts at 0");
        checkNear(easeCurve(1.0, e[0], e[1], e[2]), 1.0, "ease ends at 1");
    }
}

void linear_ease_is_close_to_identity() {
    for (double t = 0.0; t <= 1.0; t += 0.1) {
        checkNear(easeCurve(t, 0.0, 0.0, 0.0), t, "no-ease curve is identity", 1e-3);
    }
}

// Ease-out leaves slowly, so early values should stay near the start.
void ease_out_holds_near_the_start() {
    const double eased = easeCurve(0.25, 0.9, 0.0, 0.0);
    check(eased < 0.25, "heavy ease-out is behind linear at t=0.25");
}

// The assistant's OVER tile: the value sails past its target, then settles.
void overshoot_goes_past_one_then_settles() {
    bool exceeded = false;
    for (double t = 0.0; t <= 1.0; t += 0.01) {
        if (easeCurve(t, 0.0, 0.0, 0.5) > 1.0001) {
            exceeded = true;
        }
    }
    check(exceeded, "overshoot exceeds 1.0 somewhere in the segment");
    checkNear(easeCurve(1.0, 0.0, 0.0, 0.5), 1.0, "overshoot still lands on target");
}

void unanimated_property_returns_its_static_value() {
    Property p;
    p.staticValue = Value::vec2(60.0, 812.0);
    check(!p.animated(), "a property with no keys is not animated");
    check(approxEqual(p.evaluate(3.14, kAt30), Value::vec2(60.0, 812.0)),
          "unanimated property evaluates to its static value");
}

void evaluation_clamps_outside_the_keyed_range() {
    Property p;
    p.addKey({TimeValue::seconds(1.0), Value::scalar(10.0), Interpolation::Linear, 0, 0, 0},
             kAt30);
    p.addKey({TimeValue::seconds(2.0), Value::scalar(20.0), Interpolation::Linear, 0, 0, 0},
             kAt30);

    checkNear(p.evaluate(0.0, kAt30).x(), 10.0, "before the first key holds the first value");
    checkNear(p.evaluate(9.0, kAt30).x(), 20.0, "after the last key holds the last value");
    checkNear(p.evaluate(1.5, kAt30).x(), 15.0, "linear midpoint");
}

void hold_interpolation_steps() {
    Property p;
    p.addKey({TimeValue::seconds(0.0), Value::scalar(0.0), Interpolation::Hold, 0, 0, 0},
             kAt30);
    p.addKey({TimeValue::seconds(1.0), Value::scalar(100.0), Interpolation::Hold, 0, 0, 0},
             kAt30);

    checkNear(p.evaluate(0.99, kAt30).x(), 0.0, "hold does not interpolate");
    checkNear(p.evaluate(1.0, kAt30).x(), 100.0, "hold steps at the next key");
}

void keys_stay_sorted_and_replace_in_place() {
    Property p;
    p.addKey({TimeValue::seconds(2.0), Value::scalar(2.0), Interpolation::Linear, 0, 0, 0},
             kAt30);
    p.addKey({TimeValue::seconds(0.5), Value::scalar(0.5), Interpolation::Linear, 0, 0, 0},
             kAt30);
    p.addKey({TimeValue::seconds(1.0), Value::scalar(1.0), Interpolation::Linear, 0, 0, 0},
             kAt30);

    check(p.keys.size() == 3, "three distinct times give three keys");
    checkNear(p.keys[0].value.x(), 0.5, "sorted by time regardless of insertion order");
    checkNear(p.keys[1].value.x(), 1.0, "sorted, middle");
    checkNear(p.keys[2].value.x(), 2.0, "sorted, last");

    // Re-keying an existing time overwrites rather than duplicating.
    p.addKey({TimeValue::seconds(1.0), Value::scalar(99.0), Interpolation::Linear, 0, 0, 0},
             kAt30);
    check(p.keys.size() == 3, "re-keying at an existing time does not duplicate");
    checkNear(p.keys[1].value.x(), 99.0, "re-keying replaces the value");
}

// Beat-authored keyframes retime automatically with tempo.
void beat_authored_keys_retime_with_tempo() {
    Property p;
    p.addKey({TimeValue::beats(0.0), Value::scalar(0.0), Interpolation::Linear, 0, 0, 0},
             kAt30);
    p.addKey({TimeValue::beats(4.0), Value::scalar(100.0), Interpolation::Linear, 0, 0, 0},
             kAt30);

    const TimeContext slow{30.0, 90.0, true};   // 4 beats = 2.667s
    const TimeContext fast{30.0, 174.0, true};  // 4 beats = 1.379s

    checkNear(p.evaluate(2.667, slow).x(), 100.0, "one bar at 90bpm is finished by 2.667s",
              0.05);
    check(p.evaluate(2.667, fast).x() >= 100.0, "same bar at 174bpm finished much earlier");
    checkNear(p.evaluate(1.379, fast).x(), 100.0, "one bar at 174bpm is finished by 1.379s",
              0.05);
    check(p.evaluate(1.379, slow).x() < 60.0, "at 90bpm that bar is only half done");
}

// Ranges clamp on read, not on store, so an overshoot ease between legal keys stays intact.
void a_range_clamps_what_evaluation_returns() {
    TimeContext ctx;
    ctx.fps = 30.0;

    Property opacity;
    opacity.key = "opacity";
    opacity.range = ParamRange::between(0.0, 100.0);
    opacity.staticValue = Value::scalar(400.0);
    checkNear(opacity.evaluate(0.0, ctx).c[0], 100.0, "a static value over the ceiling clamps");

    opacity.staticValue = Value::scalar(-50.0);
    checkNear(opacity.evaluate(0.0, ctx).c[0], 0.0, "and under the floor");

    // Heavy overshoot between two legal keys; only the read value should be bounded.
    opacity.staticValue = Value::scalar(100.0);
    Keyframe a;
    a.time = TimeValue::seconds(0.0);
    a.value = Value::scalar(0.0);
    a.interp = Interpolation::Bezier;
    a.easeOut = 0.9;
    Keyframe b;
    b.time = TimeValue::seconds(1.0);
    b.value = Value::scalar(100.0);
    b.interp = Interpolation::Bezier;
    b.easeIn = 0.9;
    b.overshoot = 2.0;
    opacity.addKey(a, ctx);
    opacity.addKey(b, ctx);

    bool everOver = false;
    for (int i = 0; i <= 40; ++i) {
        const double t = static_cast<double>(i) / 40.0;
        const double v = opacity.evaluate(t, ctx).c[0];
        if (v > 100.0 + 1e-9 || v < -1e-9) {
            everOver = true;
        }
    }
    check(!everOver, "an overshooting curve never reads outside the range");
    checkNear(opacity.keys.back().value.c[0], 100.0, "and the stored keys are untouched");
}

// An unbounded range is the default and must not quietly bound anything.
void no_range_means_no_clamp() {
    TimeContext ctx;
    ctx.fps = 30.0;
    Property p;
    p.key = "position";
    p.staticValue = Value::vec2(-4000.0, 9000.0);
    checkNear(p.evaluate(0.0, ctx).c[0], -4000.0, "x survives");
    checkNear(p.evaluate(0.0, ctx).c[1], 9000.0, "y survives");
}

// Every component of a vector is clamped, not just the first.
void a_range_applies_to_every_component() {
    TimeContext ctx;
    ctx.fps = 30.0;
    Property p;
    p.key = "scale";
    p.range = ParamRange::between(0.0, 100.0);
    p.staticValue = Value::vec2(-10.0, 500.0);
    checkNear(p.evaluate(0.0, ctx).c[0], 0.0, "x clamps");
    checkNear(p.evaluate(0.0, ctx).c[1], 100.0, "y clamps");
}

// Drag step scales with the range, not the unit, so wide and narrow controls scrub differently.
void the_drag_step_follows_the_slider_not_the_unit() {
    const ParamRange narrow = ParamRange::between(0.0, 1.0);
    const ParamRange wide = ParamRange::atLeast(0.0, 400.0);
    check(wide.dragStep() > narrow.dragStep() * 100.0,
          "a 0-400 control scrubs far faster than a 0-1 one");
    check(narrow.dragStep() > 0.0, "and neither is zero");
}

void lerp_respects_component_count() {
    const Value a = Value::vec2(0.0, 10.0);
    const Value b = Value::vec2(100.0, 20.0);
    const Value mid = lerp(a, b, 0.5);
    check(mid.count == 2, "lerp keeps the component count");
    checkNear(mid.c[0], 50.0, "lerp component 0");
    checkNear(mid.c[1], 15.0, "lerp component 1");
}

}  // namespace

int main() {
    easing_pins_both_endpoints();
    linear_ease_is_close_to_identity();
    ease_out_holds_near_the_start();
    overshoot_goes_past_one_then_settles();
    unanimated_property_returns_its_static_value();
    evaluation_clamps_outside_the_keyed_range();
    hold_interpolation_steps();
    keys_stay_sorted_and_replace_in_place();
    beat_authored_keys_retime_with_tempo();
    lerp_respects_component_count();
    a_range_clamps_what_evaluation_returns();
    no_range_means_no_clamp();
    a_range_applies_to_every_component();
    the_drag_step_follows_the_slider_not_the_unit();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("animation: all checks passed");
    return EXIT_SUCCESS;
}
