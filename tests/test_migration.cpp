// Migration harness tests. Migrations are pure functions over the serialized param bag,
// so tests here work directly on bags with no document/registry/GPU involved.

#include <cstdio>
#include <string>

#include "ruby/core/Migration.h"
#include "ruby/engine/EffectRegistry.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

core::ParamBag::Entry entry(double v) {
    core::ParamBag::Entry e;
    e.value = core::Value::scalar(v);
    return e;
}

core::ParamBag::Entry animated(double a, double b) {
    core::ParamBag::Entry e;
    e.value = core::Value::scalar(a);
    core::Keyframe k0;
    k0.time = core::TimeValue::seconds(0.0);
    k0.value = core::Value::scalar(a);
    core::Keyframe k1;
    k1.time = core::TimeValue::seconds(1.0);
    k1.value = core::Value::scalar(b);
    e.keys = {k0, k1};
    return e;
}

// --- the bag itself ----------------------------------------------------------

// Rename must carry keyframes and expression, not just the static value.
void renaming_carries_the_whole_history() {
    core::ParamBag bag;
    core::ParamBag::Entry e = animated(0.0, 40.0);
    e.expression = "wiggle(3, 20)";
    bag.set("blur", std::move(e));

    bag.rename("blur", "radius");
    check(!bag.has("blur"), "the old key is gone");
    check(bag.has("radius"), "the new key is there");

    const core::ParamBag::Entry* moved = bag.find("radius");
    check(moved != nullptr && moved->keys.size() == 2, "both keyframes came along");
    check(moved != nullptr && moved->keys[1].value.c[0] == 40.0, "with their values");
    check(moved != nullptr && moved->expression.has_value(), "and so did the expression");
}

void renaming_something_absent_does_nothing() {
    core::ParamBag bag;
    bag.set("radius", entry(3.0));
    bag.rename("nonexistent", "radius");
    check(bag.size() == 1 && bag.find("radius")->value.c[0] == 3.0,
          "a rename from a key old content never had leaves the target alone");
}

// Rescale must hit every keyframe, not just the value at time zero.
void scaling_reaches_every_keyframe() {
    core::ParamBag bag;
    bag.set("radius", animated(0.0, 40.0));
    bag.scale("radius", 0.5);

    const core::ParamBag::Entry* e = bag.find("radius");
    check(e != nullptr && e->keys[1].value.c[0] == 20.0, "keyframes scale");
    check(e != nullptr && e->value.c[0] == 0.0, "and so does the static value");
}

// Expressions are left alone; rewriting someone's code is out of scope for a migration.
void scaling_does_not_rewrite_expressions() {
    core::ParamBag bag;
    core::ParamBag::Entry e = entry(10.0);
    e.expression = "wiggle(3, 40)";
    bag.set("radius", std::move(e));
    bag.scale("radius", 0.5);
    check(bag.find("radius")->expression == std::string("wiggle(3, 40)"),
          "the expression is untouched");
}

// --- the chain ---------------------------------------------------------------

void the_chain_runs_only_the_steps_in_range() {
    std::string trail;
    const std::vector<core::MigrationStep> steps = {
        {2, [&trail](core::ParamBag&) { trail += "2"; }},
        {3, [&trail](core::ParamBag&) { trail += "3"; }},
        {4, [&trail](core::ParamBag&) { trail += "4"; }},
        {5, [&trail](core::ParamBag&) { trail += "5"; }},
    };
    core::ParamBag bag;

    check(core::runMigrations(steps, 2, 4, bag) == 4, "reports the version reached");
    check(trail == "34", "content at 2 going to 4 runs steps 3 and 4, not 2 and not 5");

    trail.clear();
    check(core::runMigrations(steps, 5, 5, bag) == 5, "already current");
    check(trail.empty(), "and nothing ran");

    trail.clear();
    core::runMigrations(steps, 7, 4, bag);
    check(trail.empty(), "content newer than the target runs nothing");
}

// Steps run in version order, not declaration order.
void steps_run_in_version_order_however_they_were_declared() {
    std::string trail;
    const std::vector<core::MigrationStep> steps = {
        {4, [&trail](core::ParamBag&) { trail += "4"; }},
        {2, [&trail](core::ParamBag&) { trail += "2"; }},
        {3, [&trail](core::ParamBag&) { trail += "3"; }},
    };
    core::ParamBag bag;
    core::runMigrations(steps, 1, 4, bag);
    check(trail == "234", "sorted by the version each step produces");
}

// A version with no migration step is normal (defaults handle additions), not an error.
void a_version_with_no_step_is_not_a_gap() {
    const std::vector<core::MigrationStep> steps = {
        {4, [](core::ParamBag& b) { b.rename("old", "new"); }},
    };
    core::ParamBag bag;
    bag.set("old", entry(1.0));
    check(core::runMigrations(steps, 1, 4, bag) == 4,
          "1 to 4 with only a step at 4 still arrives at 4");
    check(bag.has("new"), "and the one step that existed ran");
}

// A realistic chain: rename, then rescale, on content that is animated.
void an_old_parameter_survives_two_versions_of_change() {
    const std::vector<core::MigrationStep> steps = {
        // v2 renamed `blur` to `radius`.
        {2, [](core::ParamBag& b) { b.rename("blur", "radius"); }},
        // v3 changed it from percent-of-diagonal to percent-of-width on a 16:9 assumption.
        {3, [](core::ParamBag& b) { b.scale("radius", 0.871); }},
    };
    core::ParamBag bag;
    bag.set("blur", animated(0.0, 10.0));

    core::runMigrations(steps, 1, 3, bag);
    const core::ParamBag::Entry* e = bag.find("radius");
    check(e != nullptr, "the parameter arrived under its new name");
    check(e != nullptr && std::abs(e->keys[1].value.c[0] - 8.71) < 1e-9,
          "with its animation rescaled, not just its static value");
}

// --- the registry entry point ------------------------------------------------

void a_current_instance_is_left_alone() {
    core::EffectInstance fx = engine::EffectRegistry::instance().instantiate("core.stylize.vignette");
    const std::size_t before = fx.params.size();
    const auto r = engine::EffectRegistry::instance().migrate(fx);
    check(r.ok && !r.changed(), "nothing to do for content at the current schema");
    check(r.notes.empty(), "and nothing to say about it");
    check(fx.params.size() == before, "parameters intact");
}

// Content from a newer schema is refused, not interpreted with today's schema.
void content_from_the_future_is_refused() {
    core::EffectInstance fx =
        engine::EffectRegistry::instance().instantiate("core.stylize.vignette");
    fx.schema = 99;
    fx.params[0].staticValue = core::Value::scalar(42.0);

    const auto r = engine::EffectRegistry::instance().migrate(fx);
    check(!r.ok, "refused");
    check(!r.notes.empty(), "and says why");
    check(fx.params[0].staticValue.c[0] == 42.0, "parameters left exactly as they were");
    check(fx.schema == 99, "and the version is not quietly rewritten");
}

// Unknown keys are dropped and reported (corrupt file or buggy migration).
void an_unknown_key_is_dropped_and_reported() {
    core::EffectInstance fx =
        engine::EffectRegistry::instance().instantiate("core.stylize.vignette");
    core::Property junk;
    junk.key = "wobble";
    junk.staticValue = core::Value::scalar(1.0);
    fx.params.push_back(junk);

    const auto r = engine::EffectRegistry::instance().migrate(fx);
    check(r.ok, "the instance is still usable");
    bool mentioned = false;
    for (const std::string& n : r.notes) {
        if (n.find("wobble") != std::string::npos) mentioned = true;
    }
    check(mentioned, "the unknown key is named in the report");
    for (const core::Property& p : fx.params) {
        check(p.key != "wobble", "and is not in the parameter list");
    }
}

// Missing params get filled in without disturbing values the user actually set.
void missing_parameters_are_filled_without_disturbing_the_rest() {
    core::EffectInstance fx =
        engine::EffectRegistry::instance().instantiate("core.stylize.vignette");
    fx.params[0].staticValue = core::Value::scalar(63.0);  // amount, set by hand
    fx.params.pop_back();                                   // softness, as if never written

    const auto r = engine::EffectRegistry::instance().migrate(fx);
    check(r.ok, "usable");
    check(fx.params.size() == 2, "the missing parameter came back");
    check(fx.params[0].staticValue.c[0] == 63.0, "and the one that was set is untouched");
    check(fx.params[1].key == "softness", "in schema order, not file order");
}

// Migration puts ranges back too, so a migrated instance is bounded like a fresh one.
void migration_restores_the_schema_owned_parts() {
    core::EffectInstance fx =
        engine::EffectRegistry::instance().instantiate("core.stylize.vignette");
    for (core::Property& p : fx.params) {
        p.range = core::ParamRange{};  // as a freshly parsed file arrives
        p.group.clear();
    }
    const auto r = engine::EffectRegistry::instance().migrate(fx);
    check(r.ok, "usable");
    check(fx.params[0].range.maximum.has_value() && *fx.params[0].range.maximum == 100.0,
          "the range came from the schema");
    check(!fx.params[0].group.empty(), "and so did the group");
}

// Synthetic effect at schema 3 (v1: `blur`, v2: renamed `radius`, v3: added `falloff`
// with legacy_default 0 since v1/v2 content never had it).
engine::EffectDef threeVersionEffect() {
    engine::EffectDef def;
    def.schema.id = "core.test.aged";
    def.schema.schema = 3;
    def.schema.display_name = "Aged";
    def.schema.retired_keys = {"blur"};
    def.schema.params = {
        core::ParamSpec{.key = "radius",
                        .label = "Radius",
                        .order = 0,
                        .type = core::ParamType::Float,
                        .unit = core::SpatialUnit::PercentOfWidth,
                        .default_value = 2.0,
                        .range = core::ParamRange::atLeast(0.0, 20.0),
                        .introduced_in_schema = 1},
        core::ParamSpec{.key = "falloff",
                        .label = "Falloff",
                        .order = 1,
                        .type = core::ParamType::Float,
                        .unit = core::SpatialUnit::Normalized,
                        .default_value = 0.5,
                        .legacy_default = 0.0,
                        .range = core::ParamRange::between(0.0, 1.0),
                        .introduced_in_schema = 3},
    };
    def.migrations = {
        {2, [](core::ParamBag& b) { b.rename("blur", "radius"); }},
    };
    return def;
}

// A param added later must take legacy_default for old content, not today's default
// (AE's PF_ParamFlag_USE_VALUE_FOR_OLD_PROJECTS). Improving a default must not rewrite saved work.
void old_content_takes_the_legacy_default_not_the_new_one() {
    const engine::EffectDef def = threeVersionEffect();

    core::EffectInstance old;
    old.effectId = def.schema.id;
    old.schema = 1;
    core::Property blur;
    blur.key = "blur";
    blur.staticValue = core::Value::scalar(7.0);
    old.params.push_back(blur);

    const auto r = engine::migrateAgainst(def, old);
    check(r.ok && r.changed(), "v1 content migrated");
    check(old.schema == 3, "and is stamped at the current version");
    check(old.params.size() == 2, "both parameters present");
    check(old.params[0].key == "radius" && old.params[0].staticValue.c[0] == 7.0,
          "the renamed parameter kept its value");
    check(old.params[1].key == "falloff" && old.params[1].staticValue.c[0] == 0.0,
          "and the new parameter took its LEGACY default, not 0.5");
}

// Fresh content gets the current default, not the legacy one.
void new_content_takes_the_current_default() {
    const engine::EffectDef def = threeVersionEffect();

    core::EffectInstance fresh;
    fresh.effectId = def.schema.id;
    fresh.schema = 3;
    core::Property radius;
    radius.key = "radius";
    radius.staticValue = core::Value::scalar(4.0);
    fresh.params.push_back(radius);

    const auto r = engine::migrateAgainst(def, fresh);
    check(r.ok, "usable");
    check(fresh.params[1].staticValue.c[0] == 0.5,
          "content authored today gets the current default");
}

// Retired keys are dropped silently, not reported.
void a_retired_key_is_dropped_quietly() {
    const engine::EffectDef def = threeVersionEffect();

    core::EffectInstance fx;
    fx.effectId = def.schema.id;
    fx.schema = 3;  // current, so no migration renames it
    core::Property stale;
    stale.key = "blur";  // retired
    stale.staticValue = core::Value::scalar(9.0);
    fx.params.push_back(stale);

    const auto r = engine::migrateAgainst(def, fx);
    check(r.ok, "usable");
    for (const std::string& n : r.notes) {
        check(n.find("blur") == std::string::npos,
              "a retired key is not reported at the user");
    }
    for (const core::Property& p : fx.params) {
        check(p.key != "blur", "and is not in the parameter list");
    }
}

}  // namespace

int main() {
    renaming_carries_the_whole_history();
    renaming_something_absent_does_nothing();
    scaling_reaches_every_keyframe();
    scaling_does_not_rewrite_expressions();
    the_chain_runs_only_the_steps_in_range();
    steps_run_in_version_order_however_they_were_declared();
    a_version_with_no_step_is_not_a_gap();
    an_old_parameter_survives_two_versions_of_change();
    a_current_instance_is_left_alone();
    content_from_the_future_is_refused();
    an_unknown_key_is_dropped_and_reported();
    missing_parameters_are_filled_without_disturbing_the_rest();
    migration_restores_the_schema_owned_parts();
    old_content_takes_the_legacy_default_not_the_new_one();
    new_content_takes_the_current_default();
    a_retired_key_is_dropped_quietly();

    if (failures == 0) {
        std::puts("migration: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
