#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>

#include "ruby/engine/RenderGraph.h"
#include "ruby/gpu/GpuDevice.h"

namespace ruby::engine {

// RAM tier of the preview cache. Holds each layer's finished effect output, keyed on the
// layer's node hash (not whole frames — transform hashes into the composite, so moving a
// layer only costs a recomposite, not re-running effects).
//
// Content-addressed, never explicitly invalidated: a changed input just produces a
// different key, and the stale entry ages out via LRU. No dependency tracking to get wrong.
//
// Disk tier isn't implemented here (needs GPU readback, which doesn't exist yet).
class FrameCache {
public:
    // Bytes, not entries: texture sizes vary too much (e.g. ~16MB for 1080x1920 RGBA16F)
    // for an entry count to bound memory.
    explicit FrameCache(std::size_t budgetBytes);

    // Null on a miss. A hit moves the entry to the front of the eviction order.
    [[nodiscard]] gpu::TextureHandle find(NodeHash key);

    // Ignored if a single entry exceeds the whole budget, rather than evicting everything
    // to fit something that would immediately evict itself.
    void put(NodeHash key, gpu::TextureHandle texture, std::size_t bytes);

    // Drops everything. For a resolution change or project close, where every key is
    // stale but nothing would tell the cache so.
    void clear();

    void setBudget(std::size_t bytes);

    struct Stats {
        std::size_t entries = 0;
        std::size_t bytes = 0;
        std::size_t budget = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void resetCounters() noexcept;

    // Whether a key is held, without counting as a hit or reordering it — for the ruler's
    // cache bar, so drawing it doesn't itself perturb the cache.
    [[nodiscard]] bool contains(NodeHash key) const noexcept;

private:
    struct Entry {
        NodeHash key = 0;
        gpu::TextureHandle texture;
        std::size_t bytes = 0;
    };

    void evictTo(std::size_t bytes);

    std::list<Entry> order_;  // most recently used at the front
    std::unordered_map<NodeHash, std::list<Entry>::iterator> index_;
    Stats stats_;
};

}  // namespace ruby::engine
