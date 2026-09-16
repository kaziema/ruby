#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ruby/core/Document.h"

namespace ruby::engine {

// Describes what a frame is made of before anything is drawn: each node carries a hash of
// everything beneath it, so two frames with equal hashes are the same picture. This is
// the basis of the preview cache.
//
// Building the graph is pure — no GPU, no decoding, no allocation beyond the nodes — so
// the cache can be consulted before any texture work starts.

// Where the composition's frame lands inside a target of a given size (fit + letterbox).
// Shared by the renderer and hit-testing so both agree on the same mapping.
struct FrameFit {
    double x = 0.0;      // left edge of the frame, in target pixels
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double scale = 1.0;  // target pixels per composition pixel, uniform

    [[nodiscard]] constexpr double toCompX(double px) const noexcept {
        return scale > 0.0 ? (px - x) / scale : 0.0;
    }
    [[nodiscard]] constexpr double toCompY(double py) const noexcept {
        return scale > 0.0 ? (py - y) / scale : 0.0;
    }
    [[nodiscard]] constexpr double toTargetX(double cx) const noexcept {
        return x + cx * scale;
    }
    [[nodiscard]] constexpr double toTargetY(double cy) const noexcept {
        return y + cy * scale;
    }
};

[[nodiscard]] FrameFit frameFit(double compWidth, double compHeight, double targetWidth,
                                double targetHeight) noexcept;

using NodeHash = std::uint64_t;

struct RenderNode {
    enum class Kind {
        Source,     // decoded frame, text raster, or solid color
        Effect,     // one enabled effect over the node below it
        Composite,  // all visible layers, transformed and blended
    };

    Kind kind = Kind::Source;
    core::LayerId layer = 0;  // 0 on the composite
    int effectIndex = -1;     // index into the layer's effect list, on Effect nodes
    int input = -1;           // the node feeding this one; -1 when there is none
    std::vector<int> inputs;  // the composite's layers, bottom first

    // Folds in the input's hash, this node's own params, and its source material identity.
    NodeHash hash = 0;
};

struct RenderGraph {
    std::vector<RenderNode> nodes;
    int root = -1;  // index of the Composite node, -1 when nothing is visible

    // Time is quantized to a frame index, not kept as a double: a float-second cache key
    // would miss between e.g. 1.0 and 0.9999999999 on every scrub.
    std::int64_t frame = 0;

    // The output node's hash: the identity of the finished picture.
    [[nodiscard]] NodeHash rootHash() const noexcept {
        return root >= 0 && root < static_cast<int>(nodes.size())
                   ? nodes[static_cast<std::size_t>(root)].hash
                   : 0;
    }

    // Node producing this layer's finished pixels before its transform (top of its effect
    // stack, or its source if none). What the RAM tier caches, so a transform change
    // doesn't invalidate the effect passes.
    [[nodiscard]] int outputFor(core::LayerId layer) const noexcept;
};

// Identity of content the engine can't produce itself (e.g. text, rasterized in the UI
// module) — caller supplies a number that changes when the raster would.
using ExternalKeys = std::map<core::LayerId, std::uint64_t>;

// Builds the graph for one instant. Pure: same arguments always give the same nodes/hashes.
[[nodiscard]] RenderGraph buildGraph(const core::Project& project,
                                     const core::Composition& comp, double seconds,
                                     const ExternalKeys* external = nullptr);

// FNV-1a over raw bytes, deliberately not std::hash: std::hash can differ across runs and
// stdlib versions, which is fatal for a cache key written to disk.
[[nodiscard]] NodeHash hashBytes(const void* data, std::size_t size, NodeHash seed) noexcept;
[[nodiscard]] NodeHash hashDouble(double v, NodeHash seed) noexcept;
[[nodiscard]] NodeHash hashString(const std::string& s, NodeHash seed) noexcept;

}  // namespace ruby::engine
