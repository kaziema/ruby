#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ruby/core/Document.h"
#include "ruby/core/Identity.h"
#include "ruby/core/Migration.h"

namespace ruby::engine {

// One built-in effect: schema plus the WGSL shader implementing it.
//
// Params pack into the uniform block in schema order (one vec4 each), so the shader
// indexes u.params[n] directly with no per-effect marshaling code.
struct EffectDef {
    core::EffectSchema schema;
    std::string shader;  // WGSL, fullscreen pass

    // Ordered migration steps, one per version bump. Never delete a step: old projects
    // must keep loading. Empty today since every effect is schema 1; the harness exists
    // up front because retrofitting it later, once content exists, is much harder.
    std::vector<core::MigrationStep> migrations;

    static constexpr int kMaxParams = 8;
};

// Result of migrating one instance. Always reports what changed rather than silently
// reinterpreting it.
struct EffectRegistryMigrationReport {
    bool ok = true;  // false only when the content cannot be used as is
    int from_schema = 0;
    int to_schema = 0;
    std::vector<std::string> notes;

    [[nodiscard]] bool changed() const noexcept { return from_schema != to_schema; }
};

// Same as migrate(), but against a caller-supplied definition — lets tests exercise the
// legacy_default path, which the schema-1-only built-ins never trigger.
[[nodiscard]] EffectRegistryMigrationReport migrateAgainst(const EffectDef& def,
                                                           core::EffectInstance& instance);

// Submenu for an effect, from the id's middle segment: "core.color.grade" -> Color.
// Derived rather than stored so it can't drift from the id.
[[nodiscard]] std::string effectCategory(std::string_view id);

// Fixed table of built-in effects (no plugin loader).
class EffectRegistry {
public:
    [[nodiscard]] static const EffectRegistry& instance();

    [[nodiscard]] const EffectDef* find(std::string_view id) const noexcept;
    [[nodiscard]] const std::vector<EffectDef>& all() const noexcept { return effects_; }

    // A ready-to-use instance with every parameter at its default.
    [[nodiscard]] core::EffectInstance instantiate(std::string_view id) const;

    using MigrationReport = EffectRegistryMigrationReport;

    // Brings a loaded instance up to current schema, in order:
    //   1. Run the migration chain over the file's params.
    //   2. Fill missing params: legacy_default if pre-existing, else default_value.
    //   3. Drop retired/unknown keys.
    //   4. Re-adopt ranges/groups, stamp new version.
    [[nodiscard]] MigrationReport migrate(core::EffectInstance& instance) const;

    // Restores schema-owned parts (ranges, groups) after load; project files store only
    // the values a user chose, not the effect's declared limits.
    void adoptSchema(core::EffectInstance& instance) const;

private:
    EffectRegistry();

    std::vector<EffectDef> effects_;
};

}  // namespace ruby::engine
