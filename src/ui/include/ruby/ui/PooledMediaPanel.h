#pragma once

#include <QString>
#include <QWidget>
#include <vector>

#include "ruby/io/MediaPool.h"

namespace ruby::ui {

// Every piece of media ever imported, across every project, in one folder. Painted like
// the project panel but not sharing its code: this one is app-scoped, has no
// compositions, and can show entries whose files are gone.
class PooledMediaPanel : public QWidget {
    Q_OBJECT

public:
    explicit PooledMediaPanel(QWidget* parent = nullptr);

    void setPool(const io::MediaPool* pool);
    void refresh();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    struct Row {
        QString name;
        QString type;
        QString duration;
        QString added;
        QColor swatch;
        bool missing = false;
    };

    [[nodiscard]] int rowAt(int y) const;
    void rebuild();

    const io::MediaPool* pool_ = nullptr;
    std::vector<Row> rows_;
    int selected_ = -1;
    bool folderOpen_ = true;
};

}  // namespace ruby::ui
