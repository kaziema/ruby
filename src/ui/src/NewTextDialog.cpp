#include "ruby/ui/NewTextDialog.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "ruby/ui/Theme.h"

namespace ruby::ui {

using namespace theme;

NewTextDialog::NewTextDialog(QWidget* parent)
    : QDialog(parent), color_(Qt::white), stroke_(Qt::black) {
    build(false);
}

NewTextDialog::NewTextDialog(const Settings& existing, QWidget* parent)
    : QDialog(parent), color_(existing.color), stroke_(existing.strokeColor) {
    build(true);

    text_->setPlainText(existing.text);
    font_->setCurrentFont(QFont(existing.fontFamily));
    size_->setValue(existing.fontSize);
    tracking_->setValue(existing.tracking);
    lineHeight_->setValue(existing.lineHeight);
    strokeWidth_->setValue(existing.strokeWidth);
    align_->setCurrentIndex(static_cast<int>(existing.align));
    updateSwatch(colorButton_, color_);
    updateSwatch(strokeButton_, stroke_);
}

void NewTextDialog::build(bool editing) {
    setWindowTitle(editing ? QStringLiteral("Text Settings")
                           : QStringLiteral("New Text Layer"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);

    // Text box first, and the only thing with real height — everything else is a
    // set-once knob.
    text_ = new QPlainTextEdit(this);
    text_->setPlainText(QStringLiteral("Text"));
    text_->setMinimumHeight(80);
    layout->addWidget(text_);

    auto* form = new QFormLayout;
    form->setSpacing(8);

    font_ = new QFontComboBox(this);
    form->addRow(QStringLiteral("Font"), font_);

    size_ = new QDoubleSpinBox(this);
    size_->setRange(1.0, 2000.0);
    size_->setDecimals(1);
    size_->setValue(72.0);
    size_->setSuffix(QStringLiteral(" pt"));
    form->addRow(QStringLiteral("Size"), size_);

    align_ = new QComboBox(this);
    align_->addItems({QStringLiteral("Left"), QStringLiteral("Center"),
                      QStringLiteral("Right")});
    align_->setCurrentIndex(static_cast<int>(core::TextAlign::Center));
    form->addRow(QStringLiteral("Align"), align_);

    tracking_ = new QDoubleSpinBox(this);
    tracking_->setRange(-100.0, 500.0);
    tracking_->setDecimals(1);
    tracking_->setSuffix(QStringLiteral(" pt"));
    form->addRow(QStringLiteral("Tracking"), tracking_);

    lineHeight_ = new QDoubleSpinBox(this);
    lineHeight_->setRange(0.1, 10.0);
    lineHeight_->setSingleStep(0.05);
    lineHeight_->setDecimals(2);
    lineHeight_->setValue(1.2);
    form->addRow(QStringLiteral("Line height"), lineHeight_);

    colorButton_ = new QPushButton(this);
    colorButton_->setFixedHeight(24);
    connect(colorButton_, &QPushButton::clicked, this, [this] {
        const QColor picked =
            QColorDialog::getColor(color_, this, QStringLiteral("Text Colour"));
        if (picked.isValid()) {
            color_ = picked;
            updateSwatch(colorButton_, color_);
        }
    });
    form->addRow(QStringLiteral("Colour"), colorButton_);

    strokeWidth_ = new QDoubleSpinBox(this);
    strokeWidth_->setRange(0.0, 200.0);
    strokeWidth_->setDecimals(1);
    strokeWidth_->setSuffix(QStringLiteral(" px"));
    form->addRow(QStringLiteral("Stroke"), strokeWidth_);

    strokeButton_ = new QPushButton(this);
    strokeButton_->setFixedHeight(24);
    connect(strokeButton_, &QPushButton::clicked, this, [this] {
        const QColor picked =
            QColorDialog::getColor(stroke_, this, QStringLiteral("Stroke Colour"));
        if (picked.isValid()) {
            stroke_ = picked;
            updateSwatch(strokeButton_, stroke_);
        }
    });
    form->addRow(QStringLiteral("Stroke colour"), strokeButton_);

    layout->addLayout(form);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)
        ->setText(editing ? QStringLiteral("Apply") : QStringLiteral("Create"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateSwatch(colorButton_, color_);
    updateSwatch(strokeButton_, stroke_);

    text_->setFocus();
    text_->selectAll();
}

void NewTextDialog::updateSwatch(QPushButton* button, const QColor& color) {
    button->setStyleSheet(QStringLiteral("background: %1; border: 1px solid %2;")
                              .arg(color.name(), kFieldBorder.name()));
    button->setText(color.name().toUpper());
}

NewTextDialog::Settings NewTextDialog::settings() const {
    Settings out;
    out.text = text_->toPlainText();
    out.fontFamily = font_->currentFont().family();
    out.fontSize = size_->value();
    out.tracking = tracking_->value();
    out.lineHeight = lineHeight_->value();
    out.strokeWidth = strokeWidth_->value();
    out.color = color_;
    out.strokeColor = stroke_;
    out.align = static_cast<core::TextAlign>(align_->currentIndex());
    return out;
}

}  // namespace ruby::ui
