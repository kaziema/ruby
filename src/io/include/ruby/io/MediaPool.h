#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ruby/core/Document.h"

namespace ruby::io {

// Every piece of media ever imported, across every project and session. Deliberately
// not part of a Project — scoped to the install, not the document, so project N+1 still
// knows about media from project N.
struct PooledItem {
    std::string path;  // absolute; also the identity of the entry
    std::string name;
    core::MediaKind kind = core::MediaKind::Unknown;

    double duration = 0.0;
    std::int64_t bytes = 0;

    // Unix epoch seconds, first import time. Not recoverable after the fact (file
    // timestamps don't say this), so must be captured at import.
    std::int64_t firstSeen = 0;
};

class MediaPool {
public:
    // Adds unless the path is already present (one entry per clip, however many
    // projects use it). Returns true if new.
    bool add(const PooledItem& item);

    [[nodiscard]] const std::vector<PooledItem>& items() const noexcept { return items_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

    // Best-effort, never throw: a failed load is an empty pool, not a failed launch.
    void load(const std::string& file);
    bool save(const std::string& file) const;

private:
    std::vector<PooledItem> items_;
};

}  // namespace ruby::io
