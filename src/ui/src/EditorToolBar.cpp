#include "ruby/ui/EditorToolBar.h"

#include <QFontMetrics>
#include <iterator>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QToolTip>
#include <QPainter>

#include "ruby/ui/Theme.h"
#include "ruby/ui/ToolIcons.h"

namespace ruby::ui {

using namespace theme;

namespace {

// Vector paths, not text glyphs (some render as colour emoji). AE's order minus tools
// Ruby doesn't have.
constexpr ToolIcon kTools[] = {
    ToolIcon::Selection, ToolIcon::Hand,   ToolIcon::Zoom,
    ToolIcon::Rotation,  ToolIcon::Anchor, ToolIcon::Text,
    ToolIcon::Shape,     ToolIcon::Pen,
};
constexpr int kToolCount = static_cast<int>(std::size(kTools));

}  // namespace

ToolIcon EditorToolBar::toolAt(int index) noexcept {
    return (index >= 0 && index < kToolCount)
               ? kTools[static_cast<std::size_t>(index)]
               : ToolIcon::Selection;
}

namespace {

// Panel switches, not tools: they change the left dock, not viewer clicks. Kept
// separate (own divider) so picking one doesn't deselect the active tool.
constexpr ToolIcon kPanels[] = {ToolIcon::Project, ToolIcon::Effects};
constexpr int kPanelCount = static_cast<int>(std::size(kPanels));

constexpr const char* kPanelTips[] = {
    "Project — compositions, imported media and the pooled media library",
    "Effects & Presets — search effects and presets, then drag one onto a layer",
};

// Name plus what it does; a bare name on an abstract glyph explains nothing.
constexpr const char* kToolTips[] = {
    "Selection Tool — pick, move, scale and rotate layers in the viewer",
    "Hand Tool — pan the viewer without moving anything",
    "Zoom Tool — zoom the viewer. Alt-click zooms out",
    "Rotation Tool — rotate the selected layer around its anchor point",
    "Anchor Point Tool — move a layer's origin without moving the layer",
    "Type Tool — click in the viewer to create a text layer",
    "Shape Tool — draw rectangles and ellipses. Needs shape layers, which do not exist yet",
    "Pen Tool — draw bezier paths and masks. Needs masks, which do not exist yet",
};

// Tools with nothing to act on yet: drawn dimmed and inert, like Home.
constexpr bool kToolReady[] = {true, true, true, true, true, true, false, false};

// Right side of the bar, where AE lists workspaces. Ruby has one layout, so this space
// holds app-specific feature entries instead; workspace switching is just a menu.
struct Feature {
    const char* label;
    const char* tip;
    bool ready;
};
constexpr Feature kFeatures[] = {
    {"Beat Analyzer", "Beat Analyzer — find the beats or the vocal onsets in this "
                      "composition's audio and put them on the timeline", true},
    {"Audio Studio", "Audio Studio — show only the layers you are working with, with a "
                     "mixer. Not built yet.", false},
};
constexpr int kFeatureCount = static_cast<int>(std::size(kFeatures));

constexpr int kFeaturePadX = 10;
constexpr int kWorkspaceW = 26;

constexpr int kEdgePad = 8;
constexpr int kToolGap = 1;
constexpr int kSwitchPillW = 26;
constexpr int kSwitchPillH = 13;
constexpr int kSwitchKnob = 9;

}  // namespace

EditorToolBar::EditorToolBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(metrics::kToolBarH);
    setMouseTracking(true);

    QFont f = font();
    f.setPixelSize(type::kTabLabel);
    setFont(f);

    switches_ = {
        {QStringLiteral("Snapping"), true, {}, {}},
        {QStringLiteral("Motion Blur"), false, {}, {}},
    };

    relayout();
}

void EditorToolBar::relayout() {
    const int cy = (metrics::kToolBarH - metrics::kToolButtonH) / 2;

    // Home, panel switches, tools, then the right-side switches, each run divider-
    // separated. Panel buttons sit at the far left, above the panel they control.
    int x = kEdgePad;
    homeRect_ = QRect(x, cy, metrics::kToolButtonW, metrics::kToolButtonH);
    x += metrics::kToolButtonW;

    x += 7;
    homeDividerRect_ = QRect(x, 6, 1, metrics::kToolBarH - 12);
    x += 1 + 7;

    panelRects_.clear();
    for (int i = 0; i < kPanelCount; ++i) {
        panelRects_.append(QRect(x, cy, metrics::kToolButtonW, metrics::kToolButtonH));
        x += metrics::kToolButtonW + kToolGap;
    }

    x += 7;
    panelDividerRect_ = QRect(x, 6, 1, metrics::kToolBarH - 12);
    x += 1 + 7;

    toolRects_.clear();
    for (int i = 0; i < kToolCount; ++i) {
        toolRects_.append(QRect(x, cy, metrics::kToolButtonW, metrics::kToolButtonH));
        x += metrics::kToolButtonW + kToolGap;
    }

    x += 7;
    dividerRect_ = QRect(x, 6, 1, metrics::kToolBarH - 12);
    x += 1 + 11;

    // Laid out from the right edge inward so the workspace menu stays pinned to the corner.
    const QFontMetrics rightFm(font());
    int rx = width() - kEdgePad - kWorkspaceW;
    workspaceRect_ = QRect(rx, 0, kWorkspaceW, metrics::kToolBarH);

    featureRects_.clear();
    for (int i = kFeatureCount - 1; i >= 0; --i) {
        const int w = rightFm.horizontalAdvance(QString::fromUtf8(kFeatures[i].label)) +
                      kFeaturePadX * 2;
        rx -= w;
        featureRects_.prepend(QRect(rx, 0, w, metrics::kToolBarH));
    }

    const QFontMetrics fm(font());
    for (Switch& sw : switches_) {
        const int labelW = fm.horizontalAdvance(sw.label);
        sw.labelRect = QRect(x, 0, labelW, metrics::kToolBarH);
        x += labelW + 7;
        sw.pillRect = QRect(x, (metrics::kToolBarH - kSwitchPillH) / 2, kSwitchPillW,
                            kSwitchPillH);
        x += kSwitchPillW + 16;
    }

    // Laid out from both ends, so the bar enforces its own minimum width (nothing here
    // can elide) to keep the two runs from overlapping.
    const int rightRunW = featureRects_.isEmpty()
                              ? kEdgePad + kWorkspaceW
                              : width() - featureRects_.first().left();
    const int needed = x + rightRunW + 12;
    if (minimumWidth() != needed) {
        setMinimumWidth(needed);
    }
}

void EditorToolBar::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    relayout();
}

void EditorToolBar::paintSwitch(QPainter& p, const Switch& sw) const {
    p.setPen(sw.on ? kTextSecondary : kTextDim);
    p.drawText(sw.labelRect, Qt::AlignVCenter | Qt::AlignLeft, sw.label);

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(sw.on ? kAccent : QColor("#444444"));
    const qreal r = sw.pillRect.height() / 2.0;
    p.drawRoundedRect(sw.pillRect, r, r);

    p.setBrush(sw.on ? QColor("#ffffff") : kTextDim);
    const int knobY = sw.pillRect.top() + (sw.pillRect.height() - kSwitchKnob) / 2;
    const int knobX = sw.on ? sw.pillRect.right() - kSwitchKnob - 2 : sw.pillRect.left() + 2;
    p.drawEllipse(QRect(knobX, knobY, kSwitchKnob, kSwitchKnob));
    p.setRenderHint(QPainter::Antialiasing, false);
}

void EditorToolBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kToolBar);

    // Home: dimmed since it does nothing yet — a live-looking button that ignores
    // clicks reads as broken.
    paintToolIcon(p, homeRect_, ToolIcon::Home, kTextFaint);
    p.fillRect(homeDividerRect_, kDivider);

    for (int i = 0; i < panelRects_.size(); ++i) {
        const QRect r = panelRects_.at(i);
        const bool active = (i == activePanel_);
        if (active) {
            p.fillRect(r, kAccent);
        } else if (i == hoverPanel_) {
            p.fillRect(r, kMenuActive);
        }
        paintToolIcon(p, r, kPanels[i], active ? QColor("#12212e") : kTextTertiary);
    }

    p.fillRect(panelDividerRect_, kDivider);

    for (int i = 0; i < toolRects_.size(); ++i) {
        const QRect r = toolRects_.at(i);
        const bool active = (i == activeTool_);
        const bool ready = kToolReady[i];

        if (active) {
            p.fillRect(r, kAccent);
        } else if (i == hoverTool_ && ready) {
            p.fillRect(r, kMenuActive);
        }

        paintToolIcon(p, r, kTools[i],
                      active ? QColor("#12212e") : (ready ? kTextTertiary : kTextFaint));
    }

    p.fillRect(dividerRect_, kDivider);

    // Named modes then the workspace menu, as text (not icons) like AE's workspace list.
    p.setFont(font());
    for (int i = 0; i < featureRects_.size() && i < kFeatureCount; ++i) {
        const QRect r = featureRects_.at(i);
        const bool ready = kFeatures[i].ready;
        if (i == hoverFeature_ && ready) {
            p.fillRect(r, kMenuActive);
        }
        p.setPen(ready ? kTextTertiary : kTextFaint);
        p.drawText(r, Qt::AlignCenter, QString::fromUtf8(kFeatures[i].label));
    }

    if (hoverWorkspace_) {
        p.fillRect(workspaceRect_, kMenuActive);
    }
    p.setPen(QPen(kTextTertiary, 1.0));
    const int mx = workspaceRect_.center().x();
    const int my = workspaceRect_.center().y();
    for (int row = -1; row <= 1; ++row) {
        p.drawLine(mx - 5, my + row * 4, mx + 5, my + row * 4);
    }

    for (const Switch& sw : switches_) {
        paintSwitch(p, sw);
    }

    // Machine/engine readouts live in the status bar; this bar is for things you act on.
    p.setPen(kDivider);
    p.drawLine(0, height() - 1, width(), height() - 1);
}

void EditorToolBar::mousePressEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();

    // Inert until the project selector exists; still swallows the click.
    if (homeRect_.contains(pos)) {
        return;
    }

    for (int i = 0; i < toolRects_.size(); ++i) {
        if (toolRects_.at(i).contains(pos)) {
            // A tool with nothing to act on swallows the click rather than becoming active.
            if (!kToolReady[i]) {
                return;
            }
            if (i != activeTool_) {
                activeTool_ = i;
                update();
                emit toolSelected(i);
            }
            return;
        }
    }

    for (int i = 0; i < panelRects_.size(); ++i) {
        if (panelRects_.at(i).contains(pos)) {
            if (i != activePanel_) {
                activePanel_ = i;
                update();
                emit panelSelected(i);
            }
            return;
        }
    }

    if (workspaceRect_.contains(pos)) {
        emit workspaceMenuRequested(mapToGlobal(pos));
        return;
    }
    for (int i = 0; i < featureRects_.size() && i < kFeatureCount; ++i) {
        if (featureRects_.at(i).contains(pos)) {
            // Not-yet-built features swallow the click; the tooltip explains why.
            if (kFeatures[i].ready) {
                emit featureTriggered(i);
            }
            return;
        }
    }

    for (int i = 0; i < switches_.size(); ++i) {
        Switch& sw = switches_[i];
        if (sw.pillRect.contains(pos) || sw.labelRect.contains(pos)) {
            sw.on = !sw.on;
            update();
            if (i == 0) {
                emit snappingToggled(sw.on);
            } else {
                emit motionBlurToggled(sw.on);
            }
            return;
        }
    }
}

bool EditorToolBar::event(QEvent* e) {
    // One widget, so tooltips are resolved by hit-testing rather than per-control children.
    if (e->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(e);
        const QPoint pos = help->pos();

        for (int i = 0; i < toolRects_.size() && i < kToolCount; ++i) {
            if (toolRects_.at(i).contains(pos)) {
                QToolTip::showText(help->globalPos(),
                                   QString::fromUtf8(kToolTips[i]), this);
                return true;
            }
        }
        if (homeRect_.contains(pos)) {
            QToolTip::showText(help->globalPos(),
                               QStringLiteral("Home — the project selector, once it "
                                              "exists. Does nothing yet."),
                               this);
            return true;
        }
        for (int i = 0; i < panelRects_.size() && i < kPanelCount; ++i) {
            if (panelRects_.at(i).contains(pos)) {
                QToolTip::showText(help->globalPos(),
                                   QString::fromUtf8(kPanelTips[i]), this);
                return true;
            }
        }
        for (int i = 0; i < featureRects_.size() && i < kFeatureCount; ++i) {
            if (featureRects_.at(i).contains(pos)) {
                QToolTip::showText(help->globalPos(),
                                   QString::fromUtf8(kFeatures[i].tip), this);
                return true;
            }
        }
        if (workspaceRect_.contains(pos)) {
            QToolTip::showText(help->globalPos(),
                               QStringLiteral("Workspaces — saved panel layouts"), this);
            return true;
        }
        for (const Switch& sw : switches_) {
            if (!sw.pillRect.contains(pos) && !sw.labelRect.contains(pos)) {
                continue;
            }
            const QString text =
                sw.label == QStringLiteral("Snapping")
                    ? QStringLiteral("Snapping — layer edges, the playhead and rhythm "
                                     "markers pull toward each other while dragging")
                    : QStringLiteral("Motion Blur — blur layers along their movement "
                                     "between frames");
            QToolTip::showText(help->globalPos(), text, this);
            return true;
        }

        QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

void EditorToolBar::mouseMoveEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    int hit = -1;
    for (int i = 0; i < toolRects_.size(); ++i) {
        if (toolRects_.at(i).contains(pos)) {
            hit = i;
            break;
        }
    }
    int panelHit = -1;
    for (int i = 0; i < panelRects_.size(); ++i) {
        if (panelRects_.at(i).contains(pos)) {
            panelHit = i;
            break;
        }
    }
    int featureHit = -1;
    for (int i = 0; i < featureRects_.size(); ++i) {
        if (featureRects_.at(i).contains(pos)) {
            featureHit = i;
            break;
        }
    }
    const bool workspaceHit = workspaceRect_.contains(pos);

    if (hit != hoverTool_ || panelHit != hoverPanel_ || featureHit != hoverFeature_ ||
        workspaceHit != hoverWorkspace_) {
        hoverTool_ = hit;
        hoverPanel_ = panelHit;
        hoverFeature_ = featureHit;
        hoverWorkspace_ = workspaceHit;
        update();
    }
}

void EditorToolBar::leaveEvent(QEvent*) {
    if (hoverTool_ != -1 || hoverPanel_ != -1 || hoverFeature_ != -1 || hoverWorkspace_) {
        hoverTool_ = -1;
        hoverPanel_ = -1;
        hoverFeature_ = -1;
        hoverWorkspace_ = false;
        update();
    }
}

}  // namespace ruby::ui
