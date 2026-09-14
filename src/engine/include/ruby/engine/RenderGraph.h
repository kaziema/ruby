#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ruby/core/Document.h"

namespace ruby::engine {

// What a frame is made of, described before any of it is drawn.
//
// The compositor used to walk the layer list and push quads. That works and it is fast,
// but it has no answer to the only question a cache needs answered: *what did this pixel
// depend on?* Without that there is nothing to key on, so every frame is rendered again
// from nothing, forever.
//
// So: the layer stack stays on the outside, where the user edits it, and a graph is built
// on the inside, where the renderer works. Each node carries a hash of everything beneath
// it. Two frames with the same hash are the same picture, and that is the whole basis of
// the preview cache (D9).
//
// Building the graph is PURE. No GPU, no decoding, no allocation beyond the nodes. That is
// what makes it testable, and it means a cache can be consulted before a single texture is
// touched: the cheapest render is the one that never starts.

// Where the composition's frame lands inside a target of a given size.
//
// The composition has its own aspect ratio, which is almost never the shape of the window,
// so it is fitted and letterboxed. Both the renderer and anything that has to turn a mouse
// position into a composition position need this answer, and they have to get the SAME
// answer: a click that lands two pixels from where the picture was drawn is a click on the
// wrong layer. One function, two callers, no second copy of the arithmetic.
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
        Source,     // a layer's raw material: decoded frame, text raster, or solid colour
        Effect,     // one enabled effect over the node below it
        Composite,  // every visible layer, transformed and blended into the frame
    };

    Kind kind = Kind::Source;
    core::LayerId layer = 0;  // 0 on the composite
    int effectIndex = -1;     // index into the layer's effect list, on Effect nodes
    int input = -1;           // the node feeding this one; -1 when there is none
    std::vector<int> inputs;  // the composite's layers, bottom first

    // Everything this node depends on, folded into one number: its input's hash, its own
    // parameters at this instant, and the identity of whatever material it started from.
    NodeHash hash = 0;
};

struct RenderGraph {
    std::vector<RenderNode> nodes;
    int root = -1;  // index of the Composite node, -1 when nothing is visible

    // The frame this graph describes. Time is quantised to a frame index rather than kept
    // as a double, because a cache keyed on a floating-point second would miss on the
    // difference between 1.0 and 0.9999999999, and every scrub would be a cache miss.
    std::int64_t frame = 0;

    // The output node's hash: the identity of the finished picture.
    [[nodiscard]] NodeHash rootHash() const noexcept {
        return root >= 0 && root < static_cast<int>(nodes.size())
                   ? nodes[static_cast<std::size_t>(root)].hash
                   : 0;
    }

    // The node producing this layer's finished pixels, before its transform: the top of
    // its effect stack, or its source when it has no enabled effects. This is what the RAM
    // tier caches, because a transform change must not throw away four effect passes.
    [[nodiscard]] int outputFor(core::LayerId layer) const noexcept;
};

// Identity of content the engine cannot produce itself, supplied by the caller.
//
// Text is the case: it is rasterised in the UI module because that is where the font stack
// lives. The graph cannot hash pixels it has never seen, so the caller hands over a number
// that changes when the raster would. GpuViewport already computes exactly this to decide
// when to re-rasterise, so nothing new has to be invented.
using ExternalKeys = std::map<core::LayerId, std::uint64_t>;

// Builds the graph for one instant. Pure: given the same arguments it returns the same
// nodes and the same hashes, in this run and in the next one.
[[nodiscard]] RenderGraph buildGraph(const core::Project& project,
                                     const core::Composition& comp, double seconds,
                                     const ExternalKeys* external = nullptr);

// FNV-1a over raw bytes, and deliberately not std::hash.
//
// std::hash is allowed to differ between runs and between standard library versions. That
// is harmless for a hash map and fatal for a cache written to disk, where a key computed
// today has to still name the same frame tomorrow.
[[nodiscard]] NodeHash hashBytes(const void* data, std::size_t size, NodeHash seed) noexcept;
[[nodiscard]] NodeHash hashDouble(double v, NodeHash seed) noexcept;
[[nodiscard]] NodeHash hashString(const std::string& s, NodeHash seed) noexcept;

}  // namespace ruby::engine
