#pragma once

#include <QColor>
#include <QDialog>
#include <QString>

#include "ruby/core/Document.h"

class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QPlainTextEdit;
class QPushButton;

namespace ruby::ui {

// Text settings; doubles as the editor since the inspector has no text field yet.
class NewTextDialog : public QDialog {
    Q_OBJECT

public:
    struct Settings {
        QString text;
        QString fontFamily;
        double fontSize = 72.0;
        double tracking = 0.0;
        double lineHeight = 1.2;
        double strokeWidth = 0.0;
        QColor color;
        QColor strokeColor;
        core::TextAlign align = core::TextAlign::Center;
    };

    explicit NewTextDialog(QWidget* parent = nullptr);
    NewTextDialog(const Settings& existing, QWidget* parent);

    [[nodiscard]] Settings settings() const;

private:
    void build(bool editing);
    void updateSwatch(QPushButton* button, const QColor& color);

    QColor color_;
    QColor stroke_;

    QPlainTextEdit* text_ = nullptr;
    QFontComboBox* font_ = nullptr;
    QDoubleSpinBox* size_ = nullptr;
    QDoubleSpinBox* tracking_ = nullptr;
    QDoubleSpinBox* lineHeight_ = nullptr;
    QDoubleSpinBox* strokeWidth_ = nullptr;
    QComboBox* align_ = nullptr;
    QPushButton* colorButton_ = nullptr;
    QPushButton* strokeButton_ = nullptr;
};

}  // namespace ruby::ui
