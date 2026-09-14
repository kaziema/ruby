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

// A property's contribution is its VALUE at this instant, not its keyframes.
//
// Two frames of the same animation differ; two different animations that happen to agree
// on this frame do not. Hashing the curve instead would make every frame of a keyframed
// layer miss the cache even where the value had not changed, which is precisely the case
// worth catching: a layer that holds still for two seconds should render once.
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
    // Normalised so that -0.0 and 0.0 hash the same, and so every NaN hashes the same.
    // Without this a value that is numerically identical can carry different bits and miss
    // a cache entry that is genuinely correct.
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
            best = i;  // effect nodes are appended after their source, so the last wins
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
    // Snapped back so everything downstream is evaluated at the frame's own time rather
    // than wherever the playhead happens to sit inside it. Two scrubs landing on the same
    // frame must produce the same graph, or the cache is useless during a scrub.
    const double t = static_cast<double>(graph.frame) / fps;

    // Solo is a whole-composition question, answered once before any layer is judged.
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
        // The graph only needs sizes to hash the transform, and a transform that resolves
        // through a parent needs every parent's size. Composition-sized is the right
        // approximation here: it is stable, and a wrong size would only ever cause a
        // needless miss, never a wrong hit.
        (void)l;
        return {static_cast<double>(comp.width), static_cast<double>(comp.height)};
    };

    // Bottom first, matching the draw order, so the composite hash depends on stacking.
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
            // The frame of the file being read, not the composition frame. A clip trimmed
            // to a different in point reads a different frame at the same playhead, and
            // two layers on the same clip at the same source frame are the same picture.
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
                continue;  // a disabled effect is not in the picture and not in the graph
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
            node.hash = eh;
            graph.nodes.push_back(std::move(node));
            current = static_cast<int>(graph.nodes.size()) - 1;
        }

        visible.push_back(current);

        // --- what the composite needs to know about this layer ----------------
        //
        // The transform is hashed HERE and not into the layer's own node, which is the
        // decision that makes the RAM tier worth having: moving a layer changes the
        // composite and leaves four effect passes untouched.
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
        // Still a node. An empty composition is a real picture, it is the letterbox and
        // the frame, and it is as cacheable as any other.
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
