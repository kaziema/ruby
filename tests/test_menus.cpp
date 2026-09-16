// macOS hides any QMenu with no actions, so an unpopulated menu silently vanishes from
// the bar. Asserts every menu exists and is non-empty.

#include <QApplication>
#include <QMenu>
#include <QMenuBar>
#include <cstdio>
#include <cstdlib>

#include "ruby/ui/MainWindow.h"

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
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    ruby::ui::MainWindow window;
    QMenuBar* bar = window.menuBar();

    const QStringList expected = {"File",   "Edit", "Composition", "Layer", "Effect",
                                 "Animation", "View", "Window",   "Help"};

    QStringList found;
    for (QAction* action : bar->actions()) {
        if (action->menu() != nullptr) {
            found.append(action->text());
        }
    }

    check(found == expected,
          "menu bar carries all nine menus, in order");
    if (found != expected) {
        std::fprintf(stderr, "  got: %s\n", found.join(", ").toUtf8().constData());
    }

    for (QAction* action : bar->actions()) {
        QMenu* menu = action->menu();
        if (menu == nullptr) {
            continue;
        }
        // An empty menu vanishes on macOS. Separators alone do not count.
        int commands = 0;
        for (QAction* item : menu->actions()) {
            if (!item->isSeparator()) {
                ++commands;
            }
        }
        if (commands == 0) {
            std::fprintf(stderr, "FAIL: menu \"%s\" has no commands and would be hidden "
                                 "on macOS\n",
                         action->text().toUtf8().constData());
            ++failures;
        }
    }

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("menus: all checks passed");
    return EXIT_SUCCESS;
}
