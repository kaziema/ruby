// The twirl arrow must reveal a layer's transform properties even with no keyframes yet.
// Row heights (contentHeight) are the only thing measurable from outside the class.

#include <QApplication>
#include <cstdio>
#include <cstdlib>

#include "ruby/ui/TimelineView.h"
#include "ruby/ui/Theme.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "solid", core::LayerKind::Solid).id;

    ui::TimelineView view;
    view.resize(1200, 600);
    view.setComposition(&comp);

    const int closed = view.contentHeight();
    check(closed > 0, "a closed layer has some height");

    view.toggleExpanded(id);
    const int opened = view.contentHeight();
    check(opened > closed,
          "twirling a layer with no keyframes still reveals its transform properties");

    // Header + 5 transform properties + Audio Level: anchor, position, scale, rotation,
    // opacity, audio_level (structurally present on every layer, like the speaker switch).
    const int expected = closed + ui::theme::metrics::kPropertyRowH * 7;
    check(opened == expected, "a Transform header and all six properties");

    view.toggleExpanded(id);
    check(view.contentHeight() == closed, "and closing it puts the height back");

    // U opens only what's animated; with nothing animated that's just the header.
    view.revealAnimated(id);
    const int revealed = view.contentHeight();
    check(revealed < opened, "U shows less than the twirl arrow does");
    check(revealed > closed, "but still opens the layer");

    view.revealAnimated(id);
    check(view.contentHeight() == closed, "U toggles closed again");

    {
        const core::TimeContext ctx = comp.timeContext();
        core::Property* scale = comp.find(id)->find("scale");
        scale->addKey({core::TimeValue::seconds(0.0), core::Value::vec2(100.0, 100.0),
                       core::Interpolation::Linear, 0.0, 0.0, 0.0}, ctx);
        scale->addKey({core::TimeValue::seconds(1.0), core::Value::vec2(50.0, 50.0),
                       core::Interpolation::Linear, 0.0, 0.0, 0.0}, ctx);
    }
    view.setComposition(&comp);
    view.revealAnimated(id);
    check(view.contentHeight() == closed + ui::theme::metrics::kPropertyRowH * 2,
          "U shows the header and only the animated property");

    // The twirl arrow always shows everything, even after U filtered it.
    view.toggleExpanded(id);   // closes
    view.toggleExpanded(id);   // opens, unfiltered
    check(view.contentHeight() == expected, "the arrow always means everything");

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("timelinerows: all checks passed");
    return EXIT_SUCCESS;
}
