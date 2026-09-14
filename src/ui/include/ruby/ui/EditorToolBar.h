#pragma once

#include <QList>
#include <QRect>
#include <QWidget>

#include "ruby/ui/ToolIcons.h"

namespace ruby::ui {

// The 30px tool bar: nine 24x22 tool buttons, a divider, and two labelled switches
// (Snapping, Motion Blur). Machine readouts live in the status bar instead: this bar is
// for things you act on.
//
// Custom-painted rather than assembled from QToolButtons: the sizes are exact and the
// switch is a bespoke 26x13 pill with a 9px knob.
class EditorToolBar : public QWidget {
    Q_OBJECT

public:
    explicit EditorToolBar(QWidget* parent = nullptr);

signals:
    void toolSelected(int index);

public:
    // Which tool an index means. The bar emits an index because that is what it knows;
    // anything that has to act on the choice needs the tool itself, and a second copy of
    // the table somewhere else is a second copy that can disagree.
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

    // The right side of the bar: named modes where After Effects puts workspace names,
    // then the workspace menu itself collapsed to three dashes.
    QList<QRect> featureRects_;
    QRect workspaceRect_;
    int hoverFeature_ = -1;
    bool hoverWorkspace_ = false;
    int activeTool_ = 0;
    int hoverTool_ = -1;

    // Which panel group is showing, and which one the cursor is over. Separate from the
    // tool state on purpose: picking a panel must not deselect your tool.
    int activePanel_ = 0;
    int hoverPanel_ = -1;
};

}  // namespace ruby::ui
