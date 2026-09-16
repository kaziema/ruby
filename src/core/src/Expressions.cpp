#include "ruby/core/Expressions.h"

namespace ruby::core {
namespace {

// Thread-local so each render thread owns its interpreter without a lock (see header).
thread_local ExpressionHost* g_host = nullptr;

}  // namespace

void setExpressionHost(ExpressionHost* host) { g_host = host; }
ExpressionHost* expressionHost() noexcept { return g_host; }

std::uint64_t expressionSeed(LayerId layer, std::string_view key) noexcept {
    // FNV-1a over the property key, mixed with the layer id. Both needed: without the layer
    // id, layers sharing an expression wiggle in sync; without the key, a layer's own
    // properties wiggle in sync with each other.
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : key) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    hash ^= layer + 0x9E3779B97F4A7C15ULL;

    // splitmix64 finalizer, so consecutive layer ids do not produce related seeds.
    hash = (hash ^ (hash >> 30)) * 0xBF58476D1CE4E5B9ULL;
    hash = (hash ^ (hash >> 27)) * 0x94D049BB133111EBULL;
    return hash ^ (hash >> 31);
}

Value evaluate(const Layer& layer, const Property& prop, double seconds,
               const TimeContext& ctx) {
    const Value keyframed = prop.evaluate(seconds, ctx);
    // An absent expression and an empty one mean the same thing: nothing to run.
    if (!prop.expression.has_value() || prop.expression->empty()) {
        return keyframed;
    }
    ExpressionHost* host = expressionHost();
    if (host == nullptr) {
        // No interpreter installed — tests and non-rendering tools take this path.
        return keyframed;
    }

    Value out;
    if (!host->evaluate(prop, seconds, ctx, keyframed,
                        expressionSeed(layer.id, prop.key), out)) {
        return keyframed;
    }

    // Shape mismatch keeps the keyframed value's shape rather than reinterpreting the
    // property — else a scalar on vec2 Position would silently zero out y.
    if (out.count != keyframed.count) {
        if (out.count == 1 && keyframed.count > 1) {
            // Scalar-to-vector broadcast is the one widening allowed (`value[1] * 2` on Position).
            Value widened = keyframed;
            for (int i = 0; i < widened.count; ++i) {
                widened.c[static_cast<std::size_t>(i)] = out.c[0];
            }
            return prop.clamped(widened);
        }
        return keyframed;  // already clamped by Property::evaluate
    }
    // Clamped same as a keyframe would be, or the property's range only holds until you write Lua.
    return prop.clamped(out);
}

}  // namespace ruby::core
