#include "ruby/io/History.h"

#include <utility>

#include "ruby/io/ProjectIO.h"

namespace ruby::io {

History::History(std::size_t limit) : limit_(limit == 0 ? 1 : limit) {}

void History::record(const core::Project& before, std::string label) {
    // Inside a gesture, keep only the opening snapshot; drop everything after it.
    if (gestureDepth_ > 0) {
        return;
    }
    past_.push_back({toJson(before), std::move(label)});

    // A new edit invalidates any redo future.
    future_.clear();

    if (past_.size() > limit_) {
        past_.erase(past_.begin());
    }
}

void History::beginGesture(const core::Project& before, std::string label) {
    // Nested begins collapse into one gesture. Snapshot before incrementing depth, or
    // record()'s in-gesture guard would swallow this entry too.
    if (gestureDepth_ == 0) {
        record(before, std::move(label));
    }
    ++gestureDepth_;
}

void History::endGesture() {
    if (gestureDepth_ > 0) {
        --gestureDepth_;
    }
}

std::string History::undoLabel() const {
    return past_.empty() ? std::string{} : past_.back().label;
}

std::string History::redoLabel() const {
    return future_.empty() ? std::string{} : future_.back().label;
}

bool History::undo(core::Project& current) {
    if (past_.empty()) {
        return false;
    }
    Entry entry = std::move(past_.back());
    past_.pop_back();

    core::Project restored;
    if (!fromJson(restored, entry.json).ok) {
        // Our own snapshot failing to load means the serializer is broken; drop the
        // entry rather than risk a partial-parse corruption.
        return false;
    }

    future_.push_back({toJson(current), entry.label});
    current = std::move(restored);
    return true;
}

bool History::redo(core::Project& current) {
    if (future_.empty()) {
        return false;
    }
    Entry entry = std::move(future_.back());
    future_.pop_back();

    core::Project restored;
    if (!fromJson(restored, entry.json).ok) {
        return false;
    }

    past_.push_back({toJson(current), entry.label});
    current = std::move(restored);
    return true;
}

void History::clear() {
    past_.clear();
    future_.clear();
    gestureDepth_ = 0;
}

}  // namespace ruby::io
