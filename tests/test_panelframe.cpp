// Panel frame tab-moving tests. Watch for off-by-one errors in reordering and end-of-list
// drops, since both are removal-then-insertion with the index shifting mid-operation.

#include <QApplication>
#include <QLabel>
#include <cstdio>
#include <cstdlib>

#include "ruby/ui/PanelFrame.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void checkLabels(ui::PanelFrame& frame, const QStringList& want, const char* what) {
    QStringList got;
    for (int i = 0; i < frame.tabCount(); ++i) {
        got << frame.tabLabel(i);
    }
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s\n   got  [%s]\n   want [%s]\n", what,
                     qPrintable(got.join(QStringLiteral(", "))),
                     qPrintable(want.join(QStringLiteral(", "))));
        ++failures;
    }
}

ui::PanelFrame* makeFrame(const QStringList& tabs) {
    auto* frame = new ui::PanelFrame(tabs);
    for (const QString& tab : tabs) {
        frame->addPage(new QLabel(tab));
    }
    return frame;
}

void a_tab_moves_between_frames() {
    ui::PanelFrame* a = makeFrame({QStringLiteral("Project"), QStringLiteral("Pooled")});
    ui::PanelFrame* b = makeFrame({QStringLiteral("Inspector")});

    const ui::PanelFrame::DetachedTab moved = a->takeTab(0);
    check(moved.page != nullptr, "the page came with the tab");
    check(moved.label == QStringLiteral("Project"), "and its label");
    check(a->tabCount() == 1, "the source lost one");

    b->insertTab(1, moved.label, moved.page);
    checkLabels(*b, {QStringLiteral("Inspector"), QStringLiteral("Project")},
                "the target gained it at the index asked for");
    checkLabels(*a, {QStringLiteral("Pooled")}, "and the source kept the rest");

    delete a;
    delete b;
}

// The page must travel with the tab, not whatever page sits at that index in the new frame.
void the_page_travels_with_the_tab() {
    ui::PanelFrame* a = makeFrame({QStringLiteral("One"), QStringLiteral("Two")});
    ui::PanelFrame* b = makeFrame({QStringLiteral("Other")});

    const ui::PanelFrame::DetachedTab moved = a->takeTab(1);
    auto* label = qobject_cast<QLabel*>(moved.page);
    check(label != nullptr && label->text() == QStringLiteral("Two"),
          "the page taken is the one that belonged to that tab");

    b->insertTab(0, moved.label, moved.page);
    b->setCurrentIndex(0);
    check(b->tabLabel(0) == QStringLiteral("Two"), "and it lands under its own label");

    delete a;
    delete b;
}

void a_frame_can_be_emptied() {
    ui::PanelFrame* a = makeFrame({QStringLiteral("Only")});
    const ui::PanelFrame::DetachedTab moved = a->takeTab(0);
    check(moved.page != nullptr, "the last tab comes out");
    check(a->tabCount() == 0, "leaving the frame empty rather than refusing");

    // Must not leave currentIndex pointing past the end.
    check(a->currentIndex() == 0, "and its current index is not past the end");

    delete moved.page;
    delete a;
}

void out_of_range_is_refused_rather_than_crashing() {
    ui::PanelFrame* a = makeFrame({QStringLiteral("One")});

    check(a->takeTab(-1).page == nullptr, "a negative index takes nothing");
    check(a->takeTab(9).page == nullptr, "and so does one past the end");
    check(a->tabCount() == 1, "and neither removed anything");

    a->insertTab(0, QStringLiteral("Null"), nullptr);
    check(a->tabCount() == 1, "inserting a null page does nothing");

    // A drop past the end clamps to the end (dropping on empty strip space).
    a->insertTab(99, QStringLiteral("Far"), new QLabel(QStringLiteral("Far")));
    checkLabels(*a, {QStringLiteral("One"), QStringLiteral("Far")},
                "an index past the end clamps to the end");

    delete a;
}

// A frame whose tabs are not pages, like the timeline listing open compositions.
void a_frame_can_refuse_to_give_tabs_up() {
    ui::PanelFrame* a = makeFrame({QStringLiteral("Comp 1")});
    a->setTabsMovable(false);
    check(!a->tabsMovable(), "the flag sticks");
    check(a->tabCount() == 1, "and its tab is still there");
    delete a;
}

// Renaming a tab must find its current frame, not assume a fixed home.
void a_tab_can_be_found_and_renamed_wherever_it_lives() {
    ui::PanelFrame* a = makeFrame({QStringLiteral("Comp"), QStringLiteral("Footage")});
    ui::PanelFrame* b = makeFrame({QStringLiteral("Inspector")});

    QWidget* compPage = nullptr;
    {
        int index = -1;
        const ui::PanelFrame::DetachedTab peek = a->takeTab(0);
        compPage = peek.page;
        a->insertTab(0, peek.label, peek.page);
        check(ui::PanelFrame::frameHolding(compPage, &index) == a, "found in its own frame");
        check(index == 0, "at the right index");
    }

    int index = -1;
    ui::PanelFrame* home = ui::PanelFrame::frameHolding(compPage, &index);
    check(home == a, "still in a");
    home->setTabLabel(index, QStringLiteral("Comp: Renamed"));
    check(a->tabLabel(0) == QStringLiteral("Comp: Renamed"), "renamed in place");

    // Move it, then rename again. The rename must follow the tab, not the old panel.
    const ui::PanelFrame::DetachedTab moved = a->takeTab(0);
    b->insertTab(0, moved.label, moved.page);

    home = ui::PanelFrame::frameHolding(compPage, &index);
    check(home == b, "found in the frame it moved to");
    home->setTabLabel(index, QStringLiteral("Comp: Moved"));
    check(b->tabLabel(0) == QStringLiteral("Comp: Moved"), "renamed there");
    checkLabels(*a, {QStringLiteral("Footage")},
                "and the old frame did not get the label back");

    delete a;
    delete b;
}

void a_destroyed_frame_leaves_nothing_behind() {
    QWidget* page = nullptr;
    {
        ui::PanelFrame* gone = makeFrame({QStringLiteral("Temp")});
        const ui::PanelFrame::DetachedTab taken = gone->takeTab(0);
        page = taken.page;
        delete gone;
    }
    // A destroyed frame must not still be findable by the registry.
    check(ui::PanelFrame::frameHolding(page) == nullptr,
          "a page nobody holds is held by nobody");
    delete page;
}

// Tabs share the strip width out when they stop fitting, rather than running off the edge.
void tabs_that_do_not_fit_share_the_strip_out() {
    ui::PanelFrame* frame = makeFrame({QStringLiteral("Project"),
                                       QStringLiteral("Pooled Media"),
                                       QStringLiteral("Comp Map"),
                                       QStringLiteral("Align")});
    frame->resize(220, 300);
    // Laid out on resize, so the strip has to have been through a layout pass.
    frame->show();
    QApplication::processEvents();

    // The frame insets its contents by 1px on each side so its border stays visible.
    const int strip = frame->width() - 2;
    check(strip > 0, "the strip has a width to share");

    int previousRight = 0;
    for (int i = 0; i < frame->tabCount(); ++i) {
        const QRect r = frame->tabRect(i);
        check(r.width() > 0, "every tab has some width");
        check(r.left() == previousRight, "tabs are contiguous, no gaps and no overlap");
        check(r.right() < strip + 1, "and none of them runs off the right edge");
        previousRight = r.left() + r.width();
    }
    check(previousRight == strip, "the run ends exactly at the strip's edge");

    frame->hide();
    delete frame;
}

// Tabs that fit keep their natural width instead of stretching to fill.
void tabs_that_fit_keep_their_own_width() {
    ui::PanelFrame* frame = makeFrame({QStringLiteral("A"), QStringLiteral("B")});
    frame->resize(600, 300);
    frame->show();
    QApplication::processEvents();

    int laid = 0;
    for (int i = 0; i < frame->tabCount(); ++i) {
        laid += frame->tabRect(i).width();
    }
    check(laid > 0 && laid < 300, "two short tabs do not stretch across 600px");

    frame->hide();
    delete frame;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    a_tab_moves_between_frames();
    the_page_travels_with_the_tab();
    a_frame_can_be_emptied();
    out_of_range_is_refused_rather_than_crashing();
    a_frame_can_refuse_to_give_tabs_up();
    a_tab_can_be_found_and_renamed_wherever_it_lives();
    a_destroyed_frame_leaves_nothing_behind();
    tabs_that_do_not_fit_share_the_strip_out();
    tabs_that_fit_keep_their_own_width();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("panelframe: all checks passed");
    return EXIT_SUCCESS;
}
