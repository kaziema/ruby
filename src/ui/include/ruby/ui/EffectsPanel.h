#pragma once

#include <QString>
#include <QWidget>
#include <vector>

#include "ruby/core/Document.h"

class QLineEdit;

namespace ruby::ui {

// Effects and presets, searchable, with drag onto a layer. Presets tab exists now so
// it has a home ready when the preset system lands.
class EffectsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget* parent = nullptr);

    // Which tab is showing. Driven by the panel frame's tabs rather than owned here.
    enum class Tab { Effects, Presets, ColorCorrection };
    void setTab(Tab tab);

    // MIME type carrying an effect id. Needs its own access specifier — after
    // `signals:`, moc treats everything as a signal until one appears.
    static const char* effectMimeType();

signals:
    // Double-clicked, or dropped with no target. Window applies it to the selected layer.
    void effectActivated(const std::string& effectId);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    // Category heading or effect, in one flat list rather than an always-expanded tree.
    struct Row {
        bool isCategory = false;
        QString label;
        std::string effectId;
    };

    void rebuild();
    [[nodiscard]] int rowAt(int y) const;

    Tab tab_ = Tab::Effects;
    std::vector<Row> rows_;
    QLineEdit* search_ = nullptr;
    QString filter_;
    int selected_ = -1;
    QPoint pressAt_;
    bool maybeDragging_ = false;
};

}  // namespace ruby::ui
