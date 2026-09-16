#include "ruby/engine/FrameCache.h"

namespace ruby::engine {

FrameCache::FrameCache(std::size_t budgetBytes) { stats_.budget = budgetBytes; }

gpu::TextureHandle FrameCache::find(NodeHash key) {
    const auto it = index_.find(key);
    if (it == index_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    order_.splice(order_.begin(), order_, it->second);
    ++stats_.hits;
    return it->second->texture;
}

bool FrameCache::contains(NodeHash key) const noexcept {
    return index_.find(key) != index_.end();
}

void FrameCache::put(NodeHash key, gpu::TextureHandle texture, std::size_t bytes) {
    if (texture == nullptr || bytes == 0) {
        return;
    }
    if (bytes > stats_.budget) {
        // Would evict the whole cache and still not fit; refuse rather than thrash.
        return;
    }
    if (const auto existing = index_.find(key); existing != index_.end()) {
        stats_.bytes -= existing->second->bytes;
        order_.erase(existing->second);
        index_.erase(existing);
    }

    evictTo(stats_.budget - bytes);

    order_.push_front(Entry{key, std::move(texture), bytes});
    index_[key] = order_.begin();
    stats_.bytes += bytes;
    stats_.entries = order_.size();
}

void FrameCache::evictTo(std::size_t bytes) {
    while (stats_.bytes > bytes && !order_.empty()) {
        const Entry& oldest = order_.back();
        stats_.bytes -= oldest.bytes;
        index_.erase(oldest.key);
        order_.pop_back();
        ++stats_.evictions;
    }
    stats_.entries = order_.size();
}

void FrameCache::clear() {
    order_.clear();
    index_.clear();
    stats_.bytes = 0;
    stats_.entries = 0;
}

void FrameCache::setBudget(std::size_t bytes) {
    stats_.budget = bytes;
    evictTo(bytes);
}

void FrameCache::resetCounters() noexcept {
    stats_.hits = 0;
    stats_.misses = 0;
    stats_.evictions = 0;
}

}  // namespace ruby::engine
