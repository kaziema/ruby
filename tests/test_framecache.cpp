// RAM tier of the preview cache. Mostly guards against wrong hits (showing a stale frame
// with no error), not just misses.

#include <cstdio>
#include <string>

#include "ruby/engine/FrameCache.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// Stand-in texture; the cache only holds a handle and the byte count it's told.
class FakeTexture : public gpu::Texture {
public:
    FakeTexture(std::uint32_t w, std::uint32_t h) : w_(w), h_(h) {}
    [[nodiscard]] std::uint32_t width() const noexcept override { return w_; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return h_; }
    [[nodiscard]] gpu::TextureFormat format() const noexcept override {
        return gpu::TextureFormat::RGBA16Float;
    }

private:
    std::uint32_t w_ = 0;
    std::uint32_t h_ = 0;
};

gpu::TextureHandle texture() { return std::make_shared<FakeTexture>(16, 16); }

constexpr std::size_t kEntry = 1000;

void a_miss_then_a_hit() {
    engine::FrameCache cache(10 * kEntry);
    check(cache.find(1) == nullptr, "nothing is there to begin with");
    check(cache.stats().misses == 1, "and that counted as a miss");

    cache.put(1, texture(), kEntry);
    check(cache.find(1) != nullptr, "what went in comes back out");
    check(cache.stats().hits == 1, "and that counted as a hit");
    check(cache.stats().bytes == kEntry, "the bytes are counted, not the entries");
}

// Budget is bytes, not entry count — a 1080x1920 RGBA16Float texture is ~16MB.
void the_budget_is_enforced_in_bytes() {
    engine::FrameCache cache(3 * kEntry);
    for (engine::NodeHash i = 1; i <= 3; ++i) {
        cache.put(i, texture(), kEntry);
    }
    check(cache.stats().entries == 3, "three fit");

    cache.put(4, texture(), kEntry);
    check(cache.stats().bytes <= 3 * kEntry, "the budget holds");
    check(cache.stats().evictions >= 1, "something was evicted to make room");
}

// LRU where "used" means found via find(), not just present.
void the_oldest_untouched_entry_goes_first() {
    engine::FrameCache cache(3 * kEntry);
    cache.put(1, texture(), kEntry);
    cache.put(2, texture(), kEntry);
    cache.put(3, texture(), kEntry);

    (void)cache.find(1);  // 1 is now the most recently used, 2 the least
    cache.put(4, texture(), kEntry);

    check(cache.find(1) != nullptr, "the one that was used survived");
    check(cache.find(2) == nullptr, "the one that was not is gone");
}

// contains() must not affect LRU order — the cache-bar UI polls hundreds of frames a
// second and would otherwise keep alive whatever it last scanned.
void contains_does_not_count_as_use() {
    engine::FrameCache cache(2 * kEntry);
    cache.put(1, texture(), kEntry);
    cache.put(2, texture(), kEntry);

    const auto before = cache.stats();
    check(cache.contains(1), "it is there");
    check(cache.stats().hits == before.hits, "and asking did not count as a hit");
    check(cache.stats().misses == before.misses, "nor as a miss");

    cache.put(3, texture(), kEntry);
    check(!cache.contains(1), "and it was still the least recently used, so it went");
}

// An entry bigger than the budget is refused, not allowed to evict everything for nothing.
void an_entry_too_big_for_the_budget_is_refused() {
    engine::FrameCache cache(2 * kEntry);
    cache.put(1, texture(), kEntry);
    cache.put(2, texture(), 99 * kEntry);

    check(cache.find(1) != nullptr, "the existing entry survived");
    check(!cache.contains(2), "and the oversized one was not taken");
}

// Re-putting a key replaces it rather than double-counting its bytes.
void putting_the_same_key_twice_replaces_it() {
    engine::FrameCache cache(10 * kEntry);
    cache.put(1, texture(), kEntry);
    cache.put(1, texture(), kEntry);
    check(cache.stats().entries == 1, "one entry");
    check(cache.stats().bytes == kEntry, "and its bytes counted once");
}

// Lowering the budget evicts immediately, not on next insertion.
void shrinking_the_budget_evicts_now() {
    engine::FrameCache cache(10 * kEntry);
    for (engine::NodeHash i = 1; i <= 5; ++i) {
        cache.put(i, texture(), kEntry);
    }
    cache.setBudget(2 * kEntry);
    check(cache.stats().bytes <= 2 * kEntry, "trimmed on the spot");
}

void clearing_empties_it() {
    engine::FrameCache cache(10 * kEntry);
    cache.put(1, texture(), kEntry);
    cache.clear();
    check(cache.stats().entries == 0 && cache.stats().bytes == 0, "nothing left");
    check(!cache.contains(1), "and the key is gone with it");
}

}  // namespace

int main() {
    a_miss_then_a_hit();
    the_budget_is_enforced_in_bytes();
    the_oldest_untouched_entry_goes_first();
    contains_does_not_count_as_use();
    an_entry_too_big_for_the_budget_is_refused();
    putting_the_same_key_twice_replaces_it();
    shrinking_the_budget_evicts_now();
    clearing_empties_it();

    if (failures == 0) {
        std::puts("framecache: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
