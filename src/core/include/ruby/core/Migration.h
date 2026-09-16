#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ruby/core/Animation.h"

namespace ruby::core {

// Moving old content onto a newer effect.
//
// Each effect ships an ordered list of pure functions over the serialized param bag,
// not live objects — testable without a Document/registry/GPU, and able to read keys
// the current schema has already forgotten (e.g. a rename from `blur` to `radius`).
// Migrations are kept forever: content from 2026 has to open in 2036.

// What one effect instance's parameters looked like on disk.
class ParamBag {
public:
    struct Entry {
        Value value;                        // the static value
        std::vector<Keyframe> keys;         // empty when not animated
        std::optional<std::string> expression;
    };

    [[nodiscard]] bool has(const std::string& key) const;

    // Null when absent — migrations are expected to check, since old content is often
    // missing keys.
    [[nodiscard]] const Entry* find(const std::string& key) const;
    [[nodiscard]] Entry* find(const std::string& key);

    void set(const std::string& key, Entry entry);
    void remove(const std::string& key);

    // Moves a key's whole history (keyframes + expression), not just the static value —
    // a common mistake. No-op if `from` is absent; overwrites `to` if both exist.
    void rename(const std::string& from, const std::string& to);

    // Scales static value + every keyframe/component by `factor`. Only pure-rescale
    // unit changes may happen in-place; anything else needs a new key.
    void scale(const std::string& key, double factor);

    [[nodiscard]] const std::map<std::string, Entry>& entries() const { return entries_; }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    std::map<std::string, Entry> entries_;
};

// One step. `to_schema` is the version this step PRODUCES, so a step with to_schema 3
// takes content at 2 and leaves it at 3.
struct MigrationStep {
    int to_schema = 0;
    std::function<void(ParamBag&)> apply;
};

// Runs every step needed to take `from` up to `to`. Declaration order and gaps don't
// matter (e.g. 1->4 with nothing at 2/3). Returns the version actually reached.
int runMigrations(const std::vector<MigrationStep>& steps, int from, int to, ParamBag& bag);

}  // namespace ruby::core
