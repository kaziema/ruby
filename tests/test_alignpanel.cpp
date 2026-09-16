// Align panel layout. Recurring bug class: controls laid out at natural width without
// checking available room paint off the edge unclickable (tab strip, toolbar, this).

#include <QApplication>
#include <cstdio>

#include "ruby/ui/AlignPanel.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Every button lands inside the panel, at every width the panel can be dragged to.
void the_buttons_stay_inside_the_panel() {
    ui::AlignPanel panel;
    panel.show();

    for (const int w : {268, 220, 180, 150, 120}) {
        panel.resize(w, 300);
        QApplication::processEvents();

        for (int i = 0; i < panel.buttonCount(); ++i) {
            const QRect r = panel.buttonRect(i);
            check(r.width() > 0, "a button with no width is not a button");
            check(r.left() >= 0, "no button starts off the left edge");
            if (r.right() >= w) {
                std::fprintf(stderr, "  at width %d, button %d ends at %d\n", w, i,
                             r.right());
                check(false, "a button ran off the right edge");
            }
        }
    }
    panel.hide();
}

// Align and distribute rows share the same six columns.
void the_two_rows_share_their_columns() {
    ui::AlignPanel panel;
    panel.show();
    panel.resize(200, 300);
    QApplication::processEvents();

    for (int i = 0; i < 6; ++i) {
        check(panel.buttonRect(i).left() == panel.buttonRect(i + 6).left(),
              "align and distribute share a column");
    }
    panel.hide();
}

void the_buttons_do_not_overlap() {
    ui::AlignPanel panel;
    panel.show();
    panel.resize(140, 300);
    QApplication::processEvents();

    for (int i = 0; i < 5; ++i) {
        check(panel.buttonRect(i).right() < panel.buttonRect(i + 1).left(),
              "align buttons keep off each other");
        check(panel.buttonRect(i + 6).right() < panel.buttonRect(i + 7).left(),
              "distribute buttons keep off each other");
    }
    panel.hide();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    the_buttons_stay_inside_the_panel();
    the_two_rows_share_their_columns();
    the_buttons_do_not_overlap();

    if (failures == 0) {
        std::puts("alignpanel: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
