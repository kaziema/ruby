#include "ruby/engine/RenderGraph.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "ruby/core/Expressions.h"
#include "ruby/core/Transform.h"

namespace ruby::engine {
namespace {

constexpr NodeHash kOffsetBasis = 1469598103934665603ULL;
constexpr NodeHash kPrime = 1099511628211ULL;

NodeHash hashInt(std::int64_t v, NodeHash seed) noexcept {
    return hashBytes(&v, sizeof(v), seed);
}

NodeHash hashValue(const core::Value& v, NodeHash seed) noexcept {
    NodeHash h = hashInt(v.count, seed);
    for (int i = 0; i < v.count; ++i) {
        h = hashDouble(v.c[static_cast<std::size_t>(i)], h);
    }
    return h;
}

// Hash the property's VALUE at this instant, not its keyframe curve — otherwise a layer
// that holds still for two seconds would miss cache on every frame anyway.
NodeHash hashProperty(const core::Layer& owner, const core::Property& p, double seconds,
                      const core::TimeContext& ctx, NodeHash seed) noexcept {
    NodeHash h = hashString(p.key, seed);
    return hashValue(core::evaluate(owner, p, seconds, ctx), h);
}

NodeHash hashTransform(const core::Transform2D& t, NodeHash seed) noexcept {
    NodeHash h = seed;
    for (const double v : {t.a, t.b, t.c, t.d, t.tx, t.ty}) {
        h = hashDouble(v, h);
    }
    return h;
}

}  // namespace

FrameFit frameFit(double compWidth, double compHeight, double targetWidth,
                  double targetHeight) noexcept {
    FrameFit out;
    if (compWidth <= 0.0 || compHeight <= 0.0 || targetWidth <= 0.0 ||
        targetHeight <= 0.0) {
        return out;
    }
    out.scale = std::min(targetWidth / compWidth, targetHeight / compHeight);
    out.width = compWidth * out.scale;
    out.height = compHeight * out.scale;
    out.x = (targetWidth - out.width) * 0.5;
    out.y = (targetHeight - out.height) * 0.5;
    return out;
}

NodeHash hashBytes(const void* data, std::size_t size, NodeHash seed) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    NodeHash h = seed;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= kPrime;
    }
    return h;
}

NodeHash hashDouble(double v, NodeHash seed) noexcept {
    // Normalize -0.0 -> 0.0 and all NaNs to one bit pattern, or numerically-equal values
    // hash differently and miss a valid cache entry.
    if (v == 0.0) {
        v = 0.0;
    } else if (std::isnan(v)) {
        v = std::numeric_limits<double>::quiet_NaN();
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return hashBytes(&bits, sizeof(bits), seed);
}

NodeHash hashString(const std::string& s, NodeHash seed) noexcept {
    return hashBytes(s.data(), s.size(), seed);
}

int RenderGraph::outputFor(core::LayerId layer) const noexcept {
    int best = -1;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        const RenderNode& n = nodes[static_cast<std::size_t>(i)];
        if (n.layer == layer && n.kind != RenderNode::Kind::Composite) {
            best = i;  // effect nodes append after their source; last one wins
        }
    }
    return best;
}

RenderGraph buildGraph(const core::Project& project, const core::Composition& comp,
                       double seconds, const ExternalKeys* external) {
    RenderGraph graph;
    const core::TimeContext ctx = comp.timeContext();

    const double fps = comp.fps > 0.0 ? comp.fps : 30.0;
    graph.frame = static_cast<std::int64_t>(std::llround(seconds * fps));
    // Snap back to the frame's own time: two scrubs landing on the same frame must
    // produce the same graph.
    const double t = static_cast<double>(graph.frame) / fps;

    // Solo is whole-composition, so resolve it once up front.
    bool anySolo = false;
    for (const core::Layer& layer : comp.layers) {
        if (layer.solo && layer.kind != core::LayerKind::Audio) {
            anySolo = true;
            break;
        }
    }

    std::vector<int> visible;
    NodeHash compositeHash = hashInt(comp.width, kOffsetBasis);
    compositeHash = hashInt(comp.height, compositeHash);
    compositeHash = hashDouble(fps, compositeHash);

    const core::SizeOf sizeOf = [&comp](const core::Layer& l) -> core::LayerSize {
        // Approximated as composition-sized: stable, and a wrong size can only cause a
        // needless miss, never a wrong hit.
        (void)l;
        return {static_cast<double>(comp.width), static_cast<double>(comp.height)};
    };

    // Bottom first, matching draw order, so the composite hash depends on stacking.
    for (auto it = comp.layers.rbegin(); it != comp.layers.rend(); ++it) {
        const core::Layer& layer = *it;

        if (anySolo && !layer.solo) {
            continue;
        }
        if (!layer.enabled || layer.kind == core::LayerKind::Audio ||
            layer.kind == core::LayerKind::Null) {
            continue;
        }
        const double in = to_seconds(layer.inPoint, ctx);
        const double out = to_seconds(layer.outPoint, ctx);
        if (t < in || t >= out) {
            continue;
        }

        // --- the source node -------------------------------------------------
        RenderNode source;
        source.kind = RenderNode::Kind::Source;
        source.layer = layer.id;

        NodeHash h = hashInt(static_cast<std::int64_t>(layer.kind), kOffsetBasis);
        if (const std::string path = project.pathFor(layer); !path.empty()) {
            h = hashString(path, h);
            // Source-file frame, not composition frame: a different in-point trim reads a
            // different frame at the same playhead.
            h = hashInt(static_cast<std::int64_t>(std::llround((t - in) * fps)), h);
        } else if (external != nullptr) {
            if (const auto found = external->find(layer.id); found != external->end()) {
                h = hashInt(static_cast<std::int64_t>(found->second), h);
            }
        }
        if (layer.kind == core::LayerKind::Solid) {
            h = hashValue(layer.solidColor, h);
            h = hashInt(layer.solidWidth, h);
            h = hashInt(layer.solidHeight, h);
        }
        source.hash = h;
        graph.nodes.push_back(std::move(source));
        int current = static_cast<int>(graph.nodes.size()) - 1;

        // --- one node per enabled effect -------------------------------------
        for (std::size_t e = 0; e < layer.effects.size(); ++e) {
            const core::EffectInstance& fx = layer.effects[e];
            if (!fx.enabled) {
                continue;  // disabled effects don't appear in the graph
            }
            RenderNode node;
            node.kind = RenderNode::Kind::Effect;
            node.layer = layer.id;
            node.effectIndex = static_cast<int>(e);
            node.input = current;

            NodeHash eh = graph.nodes[static_cast<std::size_t>(current)].hash;
            eh = hashString(fx.effectId, eh);
            eh = hashInt(fx.schema, eh);
            for (const core::Property& p : fx.params) {
                eh = hashProperty(layer, p, t, ctx, eh);
            }
            // Same list the compositor packs, so a None mask can't cause a needless miss
            // and an active one can't cause a stale hit.
            // Skipped entirely when empty, so mask-less effects hash exactly as before.
            const std::vector<const core::Mask*> masks = core::activeMasks(fx);
            if (!masks.empty()) {
                eh = hashInt(static_cast<std::int64_t>(masks.size()), eh);
            }
            for (const core::Mask* m : masks) {
                eh = hashInt(static_cast<std::int64_t>(m->shape), eh);
                eh = hashInt(static_cast<std::int64_t>(m->mode), eh);
                eh = hashInt(m->inverted ? 1 : 0, eh);
                for (const core::Property& p : m->params) {
                    eh = hashProperty(layer, p, t, ctx, eh);
                }
            }
            node.hash = eh;
            graph.nodes.push_back(std::move(node));
            current = static_cast<int>(graph.nodes.size()) - 1;
        }

        visible.push_back(current);

        // --- what the composite needs to know about this layer ----------------
        // Transform is hashed here, not into the layer's own node: moving a layer
        // invalidates the composite but leaves its effect passes cached.
        compositeHash = hashInt(static_cast<std::int64_t>(graph.nodes[
            static_cast<std::size_t>(current)].hash), compositeHash);
        compositeHash = hashTransform(
            core::resolvedTransform(comp, layer, t, ctx, static_cast<double>(comp.width),
                                    static_cast<double>(comp.height), sizeOf),
            compositeHash);
        compositeHash = hashInt(static_cast<std::int64_t>(layer.blend), compositeHash);
        if (const core::Property* op = layer.find("opacity"); op != nullptr) {
            compositeHash = hashProperty(layer, *op, t, ctx, compositeHash);
        }
    }

    if (visible.empty()) {
        // Still a node: an empty composition (letterbox/frame) is a real, cacheable picture.
        RenderNode empty;
        empty.kind = RenderNode::Kind::Composite;
        empty.hash = compositeHash;
        graph.nodes.push_back(std::move(empty));
        graph.root = static_cast<int>(graph.nodes.size()) - 1;
        return graph;
    }

    RenderNode composite;
    composite.kind = RenderNode::Kind::Composite;
    composite.inputs = std::move(visible);
    composite.hash = compositeHash;
    graph.nodes.push_back(std::move(composite));
    graph.root = static_cast<int>(graph.nodes.size()) - 1;
    return graph;
}

}  // namespace ruby::engine
