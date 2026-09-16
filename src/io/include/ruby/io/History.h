#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ruby/core/Document.h"

namespace ruby::io {

// Undo/redo by snapshotting the document rather than a command pattern — a snapshot
// can't be wrong about prior state the way a mismatched undo/do pair can. Cheap: a
// project is references and numbers, so a snapshot is tens of KB of text.
//
// Selection/scroll position are deliberately not captured: undoing a value shouldn't
// jump you to a different layer.
class History {
public:
    explicit History(std::size_t limit = 200);

    // Call BEFORE mutating, with a label ("Set Opacity", "Add Layer") for the undo menu.
    void record(const core::Project& before, std::string label);

    // Coalesces rapid-fire changes (e.g. a drag) into one undo step. Open before the
    // gesture, close after.
    void beginGesture(const core::Project& before, std::string label);
    void endGesture();
    [[nodiscard]] bool inGesture() const noexcept { return gestureDepth_ > 0; }

    [[nodiscard]] bool canUndo() const noexcept { return !past_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !future_.empty(); }

    // Label of the next undo/redo, for the menu. Empty when unavailable.
    [[nodiscard]] std::string undoLabel() const;
    [[nodiscard]] std::string redoLabel() const;

    // Returns false (and leaves `current` untouched) if there's nothing to do.
    bool undo(core::Project& current);
    bool redo(core::Project& current);

    void clear();

private:
    struct Entry {
        std::string json;
        std::string label;
    };

    std::vector<Entry> past_;
    std::vector<Entry> future_;
    std::size_t limit_;
    int gestureDepth_ = 0;
};

}  // namespace ruby::io
