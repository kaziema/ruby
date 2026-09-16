#pragma once

#include <optional>
#include <string>
#include <vector>

#include <QRect>
#include <QWidget>

class QLineEdit;

#include "ruby/core/Document.h"

namespace ruby::ui {

// Merged inspector: collapses AE's Transform + Effect Controls panels into one,
// showing the selected layer's property stack in collapsible groups. Custom-painted
// like the timeline — fixed row heights and scrubbable values don't fit a table view.
class InspectorView : public QWidget {
    Q_OBJECT

public:
    explicit InspectorView(QWidget* parent = nullptr);

    void setComposition(core::Composition* comp);
    void setSelectedLayer(std::optional<core::LayerId> layer);
    void setCurrentTime(double seconds);

signals:
    // Value scrubbed or typed; timeline may need to repaint (numbers, new keyframe).
    void propertyEdited();

    // Undo boundaries: window opens the gesture on press, closes it on release.
    void editBegan(const QString& label);
    void editEnded();

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

    // Right click on a property opens the expression menu (the only place to write one).
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    // Points at a property that may live on the layer itself or on one of its effects.
    // `effect` is -1 for the layer's own transform properties.
    struct PropRef {
        int effect = -1;
        int index = 0;

        [[nodiscard]] bool operator==(const PropRef& other) const noexcept {
            return effect == other.effect && index == other.index;
        }
    };

    struct GroupRow {
        std::string name;
        std::vector<PropRef> properties;
        bool expanded = true;
        int effect = -1;  // which effect this group belongs to, -1 for Transform
    };

    // One editable number on screen; a vec2 property contributes two (x, y scrubbed independently).
    struct ValueField {
        PropRef property;
        int component = 0;
        QRect rect;
        bool valid = false;
    };

    void rebuildGroups();
    [[nodiscard]] const core::Layer* layer() const;
    [[nodiscard]] core::Layer* mutableLayer();

    [[nodiscard]] const core::Property* resolve(const PropRef& ref) const;
    [[nodiscard]] core::Property* resolveMutable(const PropRef& ref);

    [[nodiscard]] const ValueField* fieldAt(const QPoint& pos) const;
    [[nodiscard]] double componentValue(const ValueField& field) const;
    void applyValue(const ValueField& field, double value);
    void commitEditor();
    void editExpression(const PropRef& ref);

    core::Composition* comp_ = nullptr;
    std::optional<core::LayerId> selected_;
    double currentTime_ = 0.0;
    std::vector<GroupRow> groups_;

    std::vector<ValueField> fields_;  // rebuilt every paint
    bool dragging_ = false;
    ValueField dragField_;
    double dragStartValue_ = 0.0;
    int dragStartX_ = 0;
    bool dragMoved_ = false;

    QLineEdit* editor_ = nullptr;
    ValueField editField_;
};

}  // namespace ruby::ui
