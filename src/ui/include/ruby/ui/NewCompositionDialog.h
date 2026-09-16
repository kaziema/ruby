#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace ruby::ui {

// Settings for a new composition. Presets first, fields second (for cases presets
// don't cover).
class NewCompositionDialog : public QDialog {
    Q_OBJECT

public:
    struct Settings {
        QString name;
        int width = 1080;
        int height = 1920;
        double fps = 30.0;
        double duration = 15.0;
    };

    explicit NewCompositionDialog(QWidget* parent = nullptr);

    // Seeded with an existing composition; the only way to shorten one, since duration
    // only grows on its own. contentEnd is shown but not enforced — a shorter duration
    // doesn't trim layers hanging past it.
    NewCompositionDialog(const Settings& existing, double contentEnd,
                         QWidget* parent = nullptr);

    [[nodiscard]] Settings settings() const;

private:
    void build(bool editing, double contentEnd);
    void applyPreset(int index);

    QLineEdit* name_ = nullptr;
    QComboBox* preset_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QComboBox* fps_ = nullptr;
    QDoubleSpinBox* duration_ = nullptr;
};

}  // namespace ruby::ui
