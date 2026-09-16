#include "ruby/ui/Theme.h"

#include <QFontDatabase>
#include <QStringList>

namespace ruby::ui::theme {
namespace {

QString firstAvailable(const QStringList& candidates, const QString& fallback) {
    const QStringList installed = QFontDatabase::families();
    for (const QString& name : candidates) {
        if (installed.contains(name, Qt::CaseInsensitive)) {
            return name;
        }
    }
    return fallback;
}

QString hex(const QColor& c) { return c.name(QColor::HexRgb); }

}  // namespace

QString uiFontFamily() {
    // Archivo, falling back to the nearest compact grotesque.
    static const QString family = firstAvailable(
        {QStringLiteral("Archivo"), QStringLiteral("Inter"), QStringLiteral("Roboto"),
         QStringLiteral("Helvetica Neue"), QStringLiteral("Segoe UI")},
        QStringLiteral("sans-serif"));
    return family;
}

QString monoFontFamily() {
    // Space Mono, for expressions and anything else that is genuinely code.
    static const QString family = firstAvailable(
        {QStringLiteral("Space Mono"), QStringLiteral("JetBrains Mono"),
         QStringLiteral("SF Mono"), QStringLiteral("Menlo"), QStringLiteral("Consolas")},
        QStringLiteral("monospace"));
    return family;
}

QPalette palette() {
    QPalette pal;

    pal.setColor(QPalette::Window, kPanelBody);
    pal.setColor(QPalette::WindowText, kTextBody);
    pal.setColor(QPalette::Base, kFieldBg);
    pal.setColor(QPalette::AlternateBase, kSubToolbar);
    pal.setColor(QPalette::Text, kTextBody);
    pal.setColor(QPalette::PlaceholderText, kTextFaint);

    pal.setColor(QPalette::Button, kButtonBg);
    pal.setColor(QPalette::ButtonText, kButtonText);
    pal.setColor(QPalette::BrightText, kTextPrimary);

    pal.setColor(QPalette::Highlight, kRowSelected);
    pal.setColor(QPalette::HighlightedText, kTextPrimary);

    pal.setColor(QPalette::ToolTipBase, kSubToolbar);
    pal.setColor(QPalette::ToolTipText, kTextPrimary);

    pal.setColor(QPalette::Light, kPanelBorder);
    pal.setColor(QPalette::Mid, kPanelBorder);
    pal.setColor(QPalette::Dark, kDivider);
    pal.setColor(QPalette::Shadow, kDivider);

    pal.setColor(QPalette::Disabled, QPalette::WindowText, kTextFaint);
    pal.setColor(QPalette::Disabled, QPalette::Text, kTextFaint);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, kTextFaint);

    return pal;
}

QFont numericFont(int pixelSize) {
    QFont f;
    f.setFamily(uiFontFamily());
    f.setPixelSize(pixelSize);
    // Tabular figures so digits don't shift width as a value changes; ignored if unsupported.
    f.setFeature(QFont::Tag("tnum"), 1);
    return f;
}

QString styleSheet() {
    // Scoped to named classes; a blanket QWidget rule would override custom-painted widgets.
    return QStringLiteral(R"(
QMenuBar {
    background: %1;
    color: %2;
    padding: 0px 4px;
}
QMenuBar::item {
    padding: 4px 9px;
    border-radius: 3px;
    background: transparent;
}
QMenuBar::item:selected, QMenuBar::item:pressed { background: %3; color: %4; }

QMenu {
    background: %1;
    color: %2;
    border: 1px solid %5;
    padding: 3px;
}
QMenu::item { padding: 4px 22px 4px 12px; border-radius: 2px; }
QMenu::item:selected { background: %3; color: %4; }
QMenu::item:disabled { color: %6; }
QMenu::separator { height: 1px; background: %5; margin: 3px 6px; }

QStatusBar {
    background: %7;
    color: %6;
    border-top: 1px solid %5;
}
QStatusBar::item { border: none; }

QToolTip {
    background: %7;
    color: %4;
    border: 1px solid %5;
    padding: 3px 6px;
}

QSplitter::handle { background: %8; }
QSplitter::handle:horizontal { width: %9px; }
QSplitter::handle:vertical { height: %9px; }

QScrollBar:vertical {
    background: %10;
    width: %11px;
    margin: 0;
    border-left: 1px solid %12;
}
QScrollBar:horizontal {
    background: %10;
    height: %11px;
    margin: 0;
    border-top: 1px solid %12;
}
QScrollBar::handle {
    background: %13;
    min-height: 30px;
    min-width: 30px;
    margin: 2px;
}
QScrollBar::handle:hover { background: %14; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)")
        .arg(hex(kMenuBar))       // 1
        .arg(hex(kMenuLabel))     // 2
        .arg(hex(kMenuActive))    // 3
        .arg(hex(kTextPrimary))   // 4
        .arg(hex(kDivider))       // 5
        .arg(hex(kTextDim))       // 6
        .arg(hex(kSubToolbar))    // 7
        .arg(hex(kGutter))              // 8
        .arg(metrics::kGutter)          // 9
        .arg(hex(kScrollTrack))         // 10
        .arg(metrics::kScrollBarW)      // 11
        .arg(hex(kDivider))             // 12
        .arg(hex(kScrollHandle))        // 13
        .arg(hex(kScrollHandleHover));  // 14
}

}  // namespace ruby::ui::theme
