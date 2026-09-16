#pragma once

#include <QWidget>
#include <vector>

namespace ruby::ui {

// Aligns/distributes layers against the composition or a selection, writing the result
// to Position. Position is stored as a % of the frame, so "align left" stays correct
// after the composition is resized.
class AlignPanel : public QWidget {
    Q_OBJECT

public:
    explicit AlignPanel(QWidget* parent = nullptr);

    enum class Align { Left, HCenter, Right, Top, VCenter, Bottom };

    // Composition is the frame's edges/centre; Selection is the bounding box of the pick
    // (needs 2+ layers).
    enum class Target { Composition, Selection };

    // Count, not bool: 0 disables everything, 1 allows align-to-composition, 3+ enables
    // Distribute.
    void setSelectionCount(int count);

    // Where a button sits. Public so callers can query actual layout, not assumed
    // geometry.
    [[nodiscard]] QRect buttonRect(int index) const;
    [[nodiscard]] int buttonCount() const;

signals:
    void alignRequested(Align edge, Target target);
    void distributeRequested(Align axis);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;
    void resizeEvent(QResizeEvent* e) override;
    bool event(QEvent* e) override;

private:
    // One button. `align` is unused on distribute buttons; distributing needs 3+ layers
    // selected.
    struct Button {
        QRect rect;
        int glyph = 0;       // index into the glyph painter
        bool distribute = false;
        Align align = Align::Left;
        const char* tip = "";
    };

    void layoutButtons();
    [[nodiscard]] int buttonAt(const QPoint& pos) const;
    [[nodiscard]] bool enabled(const Button& b) const;

    std::vector<Button> buttons_;
    QRect targetRect_;  // the "Align Layers to:" dropdown
    int labelW_ = 90;  // measured, not assumed
    int hover_ = -1;
    int selectionCount_ = 0;
    Target target_ = Target::Composition;
};

}  // namespace ruby::ui
