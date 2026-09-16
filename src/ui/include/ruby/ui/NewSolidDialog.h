#pragma once

#include <QColor>
#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;
class QSpinBox;

namespace ruby::ui {

// Solid settings: name, size, colour, and a button to snap size back to the
// composition. Currently the only way to set colour, since the inspector has none yet.
class NewSolidDialog : public QDialog {
    Q_OBJECT

public:
    struct Settings {
        QString name;
        QColor color;
        int width = 0;   // 0 means "match the composition"
        int height = 0;
    };

    // compWidth/compHeight seed the size fields and back the "Make Comp Size" button.
    NewSolidDialog(int compWidth, int compHeight, QWidget* parent = nullptr);

    [[nodiscard]] Settings settings() const;

private:
    void pickColor();
    void updateSwatch();

    int compWidth_ = 1080;
    int compHeight_ = 1920;
    QColor color_;

    QLineEdit* name_ = nullptr;
    QPushButton* swatch_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
};

}  // namespace ruby::ui
