// Undo/redo: drag coalescing, redo-branch discard on new edit, undo-with-nothing no-op.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ruby/io/History.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

core::Project makeProject() {
    core::Project p;
    core::Composition& comp = p.addComposition("c", 1080, 1920, 30.0, 12.0);
    core::Layer& layer = p.addLayer(comp, "text", core::LayerKind::Text);
    if (core::Property* op = layer.find("opacity"); op != nullptr) {
        op->staticValue = core::Value::scalar(100.0);
    }
    return p;
}

double opacityOf(const core::Project& p) {
    const core::Property* op = p.compositions().front().layers.front().find("opacity");
    return op != nullptr ? op->staticValue.x() : -1.0;
}

void setOpacity(core::Project& p, double v) {
    if (core::Property* op = p.compositions().front().layers.front().find("opacity");
        op != nullptr) {
        op->staticValue = core::Value::scalar(v);
    }
}

}  // namespace

int main() {
    core::Project project = makeProject();
    io::History history;

    check(!history.canUndo(), "a fresh history has nothing to undo");
    check(!history.canRedo(), "and nothing to redo");
    check(!history.undo(project), "undoing nothing reports that it did nothing");
    check(std::fabs(opacityOf(project) - 100.0) < 1e-9,
          "and leaves the document alone");

    history.record(project, "Set Opacity");
    setOpacity(project, 50.0);
    check(history.canUndo(), "an edit becomes undoable");
    check(history.undoLabel() == "Set Opacity", "and the menu can name it");

    check(history.undo(project), "undo reports success");
    check(std::fabs(opacityOf(project) - 100.0) < 1e-9, "and restores the old value");
    check(history.canRedo(), "which makes a redo available");
    check(history.redoLabel() == "Set Opacity", "named the same way");

    check(history.redo(project), "redo reports success");
    check(std::fabs(opacityOf(project) - 50.0) < 1e-9, "and puts the change back");

    // A new edit after undo must discard the abandoned redo branch.
    check(history.undo(project), "undo again");
    check(history.canRedo(), "redo is available before the new edit");
    history.record(project, "Set Opacity");
    setOpacity(project, 25.0);
    check(!history.canRedo(), "a new edit discards the abandoned redo branch");

    // A drag fires hundreds of changes and must be one step, not hundreds.
    core::Project dragging = makeProject();
    io::History drag;
    drag.beginGesture(dragging, "Drag Opacity");
    for (int i = 99; i >= 40; --i) {
        drag.record(dragging, "Set Opacity");  // every mouse-move would do this
        setOpacity(dragging, static_cast<double>(i));
    }
    drag.endGesture();
    check(drag.canUndo(), "the gesture is undoable");
    check(drag.undo(dragging), "undo the whole drag");
    check(std::fabs(opacityOf(dragging) - 100.0) < 1e-9,
          "one undo returns to before the drag started, not one pixel back");
    check(!drag.canUndo(), "and the drag was exactly one step");

    // A gesture that triggers another recorded edit is still one gesture.
    core::Project nested = makeProject();
    io::History nest;
    nest.beginGesture(nested, "Outer");
    nest.beginGesture(nested, "Inner");
    setOpacity(nested, 10.0);
    nest.endGesture();
    nest.endGesture();
    check(nest.canUndo() && nest.undoLabel() == "Outer",
          "nested gestures collapse into the outer one");

    // The stack is bounded, and dropping the oldest must not corrupt the rest.
    core::Project many = makeProject();
    io::History bounded(3);
    for (int i = 0; i < 10; ++i) {
        bounded.record(many, "Step");
        setOpacity(many, static_cast<double>(i));
    }
    int steps = 0;
    while (bounded.undo(many)) {
        ++steps;
    }
    check(steps == 3, "the history is capped at its limit");
    check(std::fabs(opacityOf(many) - 6.0) < 1e-9,
          "and unwinds to the oldest state it still holds");

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("history: all checks passed");
    return EXIT_SUCCESS;
}
