// Effect/parameter identity rule tests; a violation fails the build instead of breaking presets.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "ruby/core/Identity.h"

using namespace ruby::core;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

bool mentions(const std::vector<SchemaProblem>& problems, const std::string& needle) {
    for (const auto& p : problems) {
        if (p.message.find(needle) != std::string::npos ||
            p.where.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

EffectSchema good_effect() {
    EffectSchema s;
    s.id = "core.blur.directional";
    s.schema = 1;
    s.display_name = "Directional Blur";
    // Designated init: positional broke when ParamSpec grew a field in the middle.
    s.params = {
        ParamSpec{.key = "amount",
                  .label = "Amount",
                  .order = 0,
                  .type = ParamType::Float,
                  .unit = SpatialUnit::PercentOfDiagonal,
                  .default_value = 2.5,
                  .range = ParamRange::atLeast(0.0, 20.0),
                  .introduced_in_schema = 1},
        ParamSpec{.key = "angle",
                  .label = "Angle",
                  .order = 1,
                  .type = ParamType::Float,
                  .unit = SpatialUnit::Degrees,
                  .default_value = 0.0,
                  .range = ParamRange::unbounded(0.0, 360.0),
                  .introduced_in_schema = 1},
    };
    return s;
}

void a_valid_schema_has_no_problems() {
    check(validate(good_effect()).empty(), "a well-formed schema validates clean");
}

void effect_ids_must_be_namespaced() {
    EffectSchema s = good_effect();
    s.id = "blur";
    check(!validate(s).empty(), "single-segment effect id is rejected");

    s.id = "Core.Blur";
    check(!validate(s).empty(), "capitalised effect id is rejected");

    s.id = "com.vendor.thing";
    check(validate(s).empty(), "third-party namespace is accepted");
}

void parameter_keys_must_be_snake_case_and_unique() {
    EffectSchema s = good_effect();
    s.params[1].key = "Angle";
    check(mentions(validate(s), "lower_snake_case"), "capitalised param key is rejected");

    s = good_effect();
    s.params[1].key = "amount";
    check(mentions(validate(s), "duplicate"), "duplicate param key is rejected");
}

void retired_keys_can_never_be_reused() {
    EffectSchema s = good_effect();
    s.schema = 2;
    s.retired_keys = {"amount"};
    check(mentions(validate(s), "retired"), "reusing a retired key is rejected");
}

// A changed default silently rewrites saved work unless the old one is pinned.
void params_added_later_need_a_legacy_default() {
    EffectSchema s = good_effect();
    s.schema = 2;
    s.params.push_back(ParamSpec{.key = "falloff",
                                 .label = "Falloff",
                                 .order = 2,
                                 .type = ParamType::Float,
                                 .unit = SpatialUnit::Normalized,
                                 .default_value = 0.5,
                                 .range = ParamRange::between(0.0, 1.0),
                                 .introduced_in_schema = 2});
    check(mentions(validate(s), "legacy_default"),
          "param introduced after v1 without legacy_default is rejected");

    s.params.back().legacy_default = 0.0;
    check(validate(s).empty(), "same param with a legacy_default validates clean");
}

// An unusable range that still looks fine visually is worse than an obviously broken one.
void nonsense_ranges_are_rejected() {
    {
        EffectSchema s = good_effect();
        s.params[0].range = ParamRange{5.0, 1.0, 5.0, 1.0};
        check(mentions(validate(s), "above its maximum"), "inverted hard range");
    }
    {
        EffectSchema s = good_effect();
        s.params[0].range = ParamRange{0.0, 10.0, 4.0, 4.0};
        check(mentions(validate(s), "cannot be dragged"), "slider with no span");
    }
    {
        // Slider running past the clamp: the last stretch of it would do nothing.
        EffectSchema s = good_effect();
        s.params[0].range = ParamRange{0.0, 10.0, 0.0, 50.0};
        check(mentions(validate(s), "above the hard maximum"), "slider past the clamp");
    }
    {
        EffectSchema s = good_effect();
        s.params[0].range = ParamRange::between(0.0, 1.0);  // default_value is 2.5
        check(mentions(validate(s), "default_value is outside"), "default outside range");
    }
    {
        EffectSchema s = good_effect();
        s.schema = 2;
        s.params[0].legacy_default = 40.0;
        s.params[0].range = ParamRange::between(0.0, 10.0);
        s.params[0].default_value = 5.0;
        check(mentions(validate(s), "legacy_default is outside"),
              "a legacy default outside the range would silently rewrite old content");
    }
}

// Widening a range is free; tightening one changes stored values and needs a schema bump.
void tightening_a_range_costs_a_bump() {
    const EffectSchema before = good_effect();

    EffectSchema tighter = good_effect();
    tighter.params[0].range = ParamRange::between(0.0, 5.0);  // was 0 or greater
    check(mentions(validate_against_previous(before, tighter), "tightened"),
          "adding a ceiling at the same schema version is rejected");

    tighter.schema = 2;
    check(!mentions(validate_against_previous(before, tighter), "tightened"),
          "the same change with a bump is fine");

    EffectSchema wider = good_effect();
    wider.params[0].range = ParamRange::atLeast(0.0, 40.0);  // slider only
    check(!mentions(validate_against_previous(before, wider), "tightened"),
          "moving where the slider ends is display only and costs nothing");

    EffectSchema loosened = good_effect();
    loosened.params[0].range = ParamRange::unbounded(0.0, 20.0);  // floor removed
    check(!mentions(validate_against_previous(before, loosened), "tightened"),
          "removing a floor cannot invalidate a stored value");
}

void ids_and_types_are_immortal() {
    const EffectSchema before = good_effect();

    EffectSchema after = before;
    after.id = "core.blur.direction";
    check(mentions(validate_against_previous(before, after), "immortal"),
          "renaming an effect id is caught");

    after = before;
    after.schema = 2;
    after.params[0].type = ParamType::Int;
    check(mentions(validate_against_previous(before, after), "type changed"),
          "retyping a parameter is caught");

    after = before;
    after.schema = 2;
    after.params[0].unit = SpatialUnit::Px;
    check(mentions(validate_against_previous(before, after), "unit changed"),
          "changing a parameter's unit is caught");
}

void removing_a_param_requires_retiring_its_key() {
    const EffectSchema before = good_effect();

    EffectSchema after = before;
    after.schema = 2;
    after.params.pop_back();
    check(mentions(validate_against_previous(before, after), "retired_keys"),
          "removing a param without retiring its key is caught");

    after.retired_keys = {"angle"};
    check(validate_against_previous(before, after).empty(),
          "removing a param and retiring its key is fine");
}

void changing_a_default_requires_pinning_the_old_one() {
    const EffectSchema before = good_effect();

    EffectSchema after = before;
    after.schema = 2;
    after.params[0].default_value = 4.0;
    check(mentions(validate_against_previous(before, after), "legacy_default"),
          "moving a default without pinning the old one is caught");

    after.params[0].legacy_default = 2.5;  // the old default
    check(validate_against_previous(before, after).empty(),
          "moving a default while pinning the old one is fine");
}

}  // namespace

int main() {
    a_valid_schema_has_no_problems();
    nonsense_ranges_are_rejected();
    tightening_a_range_costs_a_bump();
    effect_ids_must_be_namespaced();
    parameter_keys_must_be_snake_case_and_unique();
    retired_keys_can_never_be_reused();
    params_added_later_need_a_legacy_default();
    ids_and_types_are_immortal();
    removing_a_param_requires_retiring_its_key();
    changing_a_default_requires_pinning_the_old_one();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("identity: all checks passed");
    return EXIT_SUCCESS;
}
