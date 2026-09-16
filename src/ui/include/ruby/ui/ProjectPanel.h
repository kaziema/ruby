#pragma once

#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <string>
#include <vector>

#include "ruby/core/Document.h"

class QLineEdit;

namespace ruby::ui {

// Compositions and imported media in a three-column list. Custom-painted like the
// timeline and inspector: fixed rows, alternating backgrounds, swatch per row, footer.
class ProjectPanel : public QWidget {
    Q_OBJECT

public:
    explicit ProjectPanel(QWidget* parent = nullptr);

    void setProject(const core::Project* project);

    // Call after anything changes the pool or the comp list.
    void refresh();

signals:
    // Double-clicked a media item: put it in the current composition.
    void mediaActivated(core::MediaId media);
    void compositionActivated(core::CompId comp);

    // Footer buttons; the window handles these since they're undoable document edits.
    void newCompositionRequested();

    // Footage dropped on the New Composition button: make a composition that matches it.
    void compositionFromMediaRequested(core::MediaId media);
    void deleteRequested(bool isComposition, std::uint64_t id);

public:
    // MIME type carrying a MediaId. Needs its own access specifier — after `signals:`,
    // moc treats everything as a signal until one appears.
    static const char* mediaMimeType();

protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    struct Row {
        bool isComposition = false;
        std::uint64_t id = 0;
        QString name;
        QString type;
        QString duration;
        QColor swatch;
        qint64 bytes = 0;

        // Resolution, frame rate, path — shown on hover since the panel is too narrow.
        QString detail;
    };

    [[nodiscard]] int rowAt(int y) const;
    void rebuild();
    void layoutFooter();

    // Left to right along the footer. Deliberately few buttons.
    QRect newCompRect_;
    QRect deleteRect_;
    QRect countRect_;
    QRect sizeRect_;
    int hoverButton_ = -1;
    bool dropOnNewComp_ = false;

    const core::Project* project_ = nullptr;
    std::vector<Row> rows_;
    QLineEdit* search_ = nullptr;
    QString filter_;
    int selected_ = -1;
    QPoint pressAt_;
    bool maybeDragging_ = false;
    qint64 totalBytes_ = 0;
};

}  // namespace ruby::ui
