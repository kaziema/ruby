#pragma once

#include <cstdint>
#include <string>

#include "ruby/core/Document.h"

namespace ruby::core {

// The seam between the document and whatever runs expressions. Core has no dependency
// on Lua and never will — it keeps core fast to test and readable without an
// interpreter. Core declares the interface; the script module supplies it. No host
// installed just evaluates keyframes (the pre-expressions behavior), so `test_document`
// runs with no interpreter anywhere.
class ExpressionHost {
public:
    virtual ~ExpressionHost() = default;

    // Returns false to mean "use the keyframed value" — a failed expression shouldn't
    // stop a frame from drawing. The whole Property is passed (not just source text)
    // since things like `loopOut` need the keyframes themselves.
    [[nodiscard]] virtual bool evaluate(const Property& prop, double seconds,
                                        const TimeContext& ctx, const Value& fallback,
                                        std::uint64_t seed, Value& out) = 0;
};

// Thread-local, deliberately: a Lua state belongs to one thread, and a shared one
// would need a lock in the hot path of every animated property.
void setExpressionHost(ExpressionHost* host);
[[nodiscard]] ExpressionHost* expressionHost() noexcept;

// A property's value, running its expression if present and a host is installed.
// The layer is here only for the seed: two layers with the same expression must wiggle
// differently, and one layer must wiggle the same on every render.
[[nodiscard]] Value evaluate(const Layer& layer, const Property& prop, double seconds,
                             const TimeContext& ctx);

// Seed for one property on one layer. Exposed for direct testing — instability here
// changes every wiggle in every project.
[[nodiscard]] std::uint64_t expressionSeed(LayerId layer, std::string_view key) noexcept;

}  // namespace ruby::core
