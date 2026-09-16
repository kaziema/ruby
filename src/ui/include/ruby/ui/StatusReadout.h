#pragma once

#include <QColor>
#include <QString>
#include <QWidget>
#include <vector>

namespace ruby::ui {

// Right end of the status bar. A list, not fixed labels, so new readouts (CPU, cache,
// decode backlog, etc.) are one line at the call site rather than a new widget.
class StatusReadout : public QWidget {
    Q_OBJECT

public:
    struct Item {
        QString text;
        QColor dot;       // invalid means no dot
        bool warn = false;  // draws in the warning colour rather than the dim one
    };

    explicit StatusReadout(QWidget* parent = nullptr);

    void setItems(std::vector<Item> items);

protected:
    void paintEvent(QPaintEvent*) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    std::vector<Item> items_;
};

}  // namespace ruby::ui
