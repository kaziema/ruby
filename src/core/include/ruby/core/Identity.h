#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruby/core/Units.h"

namespace ruby::core {

// --- Effect and parameter identity -------------------------------------------
//
// Display order (freely rearrangeable) is separate from persistent identity
// (immortal string keys, unlike AE's integer disk IDs).
//
// THE RULES, non-negotiable once shipped:
//   - A key is immortal. Never rename, retype, or change its meaning.
//   - Deleting a parameter retires its key permanently (see retired_keys);
//     a retired key can never be reused.
//   - Adding is append-only, with a default.
//   - Two defaults: `default_value` for new instances, `legacy_default` for
//     content predating the parameter or its default change — else improving
//     a default silently rewrites saved work.
//   - Changing a parameter's meaning is forbidden. Add a new key, write a migration.

enum class ParamType {
    Float,
    Int,
    Bool,
    Color,
    Point2,
    Enum,
    Text,
    Curve,
};

struct ParamSpec {
    std::string key;                       // immortal, snake_case
    std::string label;                     // free to change, localizable
    int order = 0;                         // display order, free to change
    ParamType type = ParamType::Float;
    SpatialUnit unit = SpatialUnit::Normalized;

    double default_value = 0.0;            // for new instances
    std::optional<double> legacy_default;  // for content predating this param

    // Hard limits and slider ends (see ParamRange). Hard limits are part of the contract:
    // tightening needs a schema bump + migration; widening is always safe. Slider range
    // and group are display-only, free to change like `order`/`label`.
    ParamRange range;

    // Which section this sits under in the inspector/timeline. Empty means the effect's
    // own name.
    std::string group;

    int introduced_in_schema = 1;
};

struct EffectSchema {
    std::string id;            // "core.blur.directional" — immortal
    int schema = 1;            // bumped on any parameter change
    std::string display_name;  // free to change

    std::vector<ParamSpec> params;
    std::vector<std::string> retired_keys;  // permanently unusable

    [[nodiscard]] const ParamSpec* find(std::string_view key) const noexcept;
    [[nodiscard]] bool is_retired(std::string_view key) const noexcept;
};

// --- Schema validation -------------------------------------------------------
//
// Makes the rules above executable: every registered effect runs through this in
// tests, so a violation fails the build instead of breaking presets silently.

struct SchemaProblem {
    std::string where;    // effect id, or "<effect id>.<param key>"
    std::string message;
};

[[nodiscard]] std::vector<SchemaProblem> validate(const EffectSchema& s);

// Compares a newly edited schema against the last shipped one; reports anything that
// would break existing presets (renamed/retyped params, reused retired keys, silently
// changed defaults).
[[nodiscard]] std::vector<SchemaProblem> validate_against_previous(
    const EffectSchema& previous, const EffectSchema& current);

}  // namespace ruby::core
