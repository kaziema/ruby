#pragma once

#include <QList>
#include <QRect>
#include <QWidget>

#include "ruby/ui/ToolIcons.h"

namespace ruby::ui {

// Tool bar: tool buttons, a divider, and labelled switches (Snapping, Motion Blur).
// Custom-painted rather than QToolButtons, for exact sizes and the bespoke switch pill.
class EditorToolBar : public QWidget {
    Q_OBJECT

public:
    explicit EditorToolBar(QWidget* parent = nullptr);

signals:
    void toolSelected(int index);

public:
    // Maps a bar index to its tool. Avoids duplicating the tool table elsewhere.
    [[nodiscard]] static ToolIcon toolAt(int index) noexcept;

signals:

    // Which panel group the left dock should show. 0 is Project, 1 is Effects & Presets.
    void panelSelected(int index);

    // A named mode from the right side of the bar. 0 is Beat Analyzer, 1 is Audio Studio.
    void featureTriggered(int index);

    // The workspace menu, collapsed to three dashes at the far right.
    void workspaceMenuRequested(const QPoint& globalPos);
    void snappingToggled(bool on);
    void motionBlurToggled(bool on);

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    struct Switch {
        QString label;
        bool on;
        QRect labelRect;
        QRect pillRect;
    };

    void relayout();
    void paintSwitch(QPainter& p, const Switch& sw) const;

    QList<QRect> toolRects_;
    QList<QRect> panelRects_;
    QList<Switch> switches_;
    QRect dividerRect_;
    QRect panelDividerRect_;
    QRect homeRect_;
    QRect homeDividerRect_;

    // Right side of the bar: named modes, then the workspace menu (three dashes).
    QList<QRect> featureRects_;
    QRect workspaceRect_;
    int hoverFeature_ = -1;
    bool hoverWorkspace_ = false;
    int activeTool_ = 0;
    int hoverTool_ = -1;

    // Active/hovered panel, kept separate from tool state — picking a panel must not
    // deselect the tool.
    int activePanel_ = 0;
    int hoverPanel_ = -1;
};

}  // namespace ruby::ui
