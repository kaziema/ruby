#include "ruby/ui/NewCompositionDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "ruby/ui/Theme.h"

namespace ruby::ui {
namespace {

struct Preset {
    const char* label;
    int width;
    int height;
};

// Vertical first: this is a short-form editor, so that's the common case.
constexpr Preset kPresets[] = {
    {"Vertical  ·  1080 x 1920  ·  TikTok, Reels, Shorts", 1080, 1920},
    {"Square  ·  1080 x 1080", 1080, 1080},
    {"Landscape  ·  1920 x 1080", 1920, 1080},
    {"Landscape 4K  ·  3840 x 2160", 3840, 2160},
    {"Custom", 0, 0},
};

}  // namespace

NewCompositionDialog::NewCompositionDialog(QWidget* parent) : QDialog(parent) {
    build(false, 0.0);
}

NewCompositionDialog::NewCompositionDialog(const Settings& existing, double contentEnd,
                                           QWidget* parent)
    : QDialog(parent) {
    build(true, contentEnd);

    name_->setText(existing.name);
    width_->setValue(existing.width);
    height_->setValue(existing.height);
    fps_->setCurrentText(QString::number(existing.fps, 'g', 5));
    duration_->setValue(existing.duration);
    name_->selectAll();
}

void NewCompositionDialog::build(bool editing, double contentEnd) {
    setWindowTitle(editing ? QStringLiteral("Composition Settings")
                           : QStringLiteral("New Composition"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setSpacing(8);

    name_ = new QLineEdit(QStringLiteral("Comp 1"), this);
    form->addRow(QStringLiteral("Name"), name_);

    preset_ = new QComboBox(this);
    for (const Preset& p : kPresets) {
        preset_->addItem(QString::fromUtf8(p.label));
    }
    form->addRow(QStringLiteral("Preset"), preset_);

    width_ = new QSpinBox(this);
    width_->setRange(16, 16384);
    width_->setSingleStep(2);
    form->addRow(QStringLiteral("Width"), width_);

    height_ = new QSpinBox(this);
    height_->setRange(16, 16384);
    height_->setSingleStep(2);
    form->addRow(QStringLiteral("Height"), height_);

    fps_ = new QComboBox(this);
    // 23.976/29.97 included since real footage arrives at them.
    for (const char* rate : {"23.976", "24", "25", "29.97", "30", "50", "60"}) {
        fps_->addItem(QString::fromUtf8(rate));
    }
    fps_->setCurrentText(QStringLiteral("30"));
    form->addRow(QStringLiteral("Frame rate"), fps_);

    duration_ = new QDoubleSpinBox(this);
    // Cap is deliberately huge: duration grows to fit dropped clips, and this dialog
    // is the only way to shrink it back down.
    duration_->setRange(0.5, 86400.0);
    duration_->setDecimals(2);
    duration_->setSuffix(QStringLiteral(" s"));
    duration_->setValue(15.0);
    form->addRow(QStringLiteral("Duration"), duration_);

    layout->addLayout(form);

    // Shows where the last layer ends, since that's the number needed to shrink safely.
    if (editing && contentEnd > 0.0) {
        auto* hint = new QLabel(
            QStringLiteral("Layers run to %1 s. A shorter duration keeps them, it just "
                           "stops rendering there.")
                .arg(contentEnd, 0, 'f', 2),
            this);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
        layout->addWidget(hint);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    buttons->button(QDialogButtonBox::Ok)->setText(editing ? QStringLiteral("Apply")
                                                          : QStringLiteral("Create"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(preset_, &QComboBox::currentIndexChanged, this,
            &NewCompositionDialog::applyPreset);

    // Switches to Custom when width/height stop matching any preset.
    const auto markCustom = [this] {
        for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i) {
            if (kPresets[i].width == width_->value() &&
                kPresets[i].height == height_->value()) {
                preset_->setCurrentIndex(i);
                return;
            }
        }
        preset_->setCurrentIndex(static_cast<int>(std::size(kPresets)) - 1);
    };
    connect(width_, &QSpinBox::valueChanged, this, markCustom);
    connect(height_, &QSpinBox::valueChanged, this, markCustom);

    if (!editing) {
        applyPreset(0);
    }
    name_->setFocus();
    name_->selectAll();
}

void NewCompositionDialog::applyPreset(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kPresets))) {
        return;
    }
    const Preset& preset = kPresets[index];
    if (preset.width == 0) {
        return;  // Custom leaves whatever is already there
    }
    QSignalBlocker blockWidth(width_);
    QSignalBlocker blockHeight(height_);
    width_->setValue(preset.width);
    height_->setValue(preset.height);
}

NewCompositionDialog::Settings NewCompositionDialog::settings() const {
    Settings out;
    out.name = name_->text().trimmed();
    if (out.name.isEmpty()) {
        out.name = QStringLiteral("Comp");
    }
    out.width = width_->value();
    out.height = height_->value();
    out.fps = fps_->currentText().toDouble();
    out.duration = duration_->value();
    return out;
}

}  // namespace ruby::ui
