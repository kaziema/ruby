#include "ruby/ui/NewSolidDialog.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "ruby/ui/Theme.h"

namespace ruby::ui {

using namespace theme;

NewSolidDialog::NewSolidDialog(int compWidth, int compHeight, QWidget* parent)
    : QDialog(parent),
      compWidth_(compWidth > 0 ? compWidth : 1080),
      compHeight_(compHeight > 0 ? compHeight : 1920),
      // Mid grey: visible against the near-black frame surround and against most footage.
      color_(QColor(128, 128, 128)) {
    setWindowTitle(QStringLiteral("Solid Settings"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setSpacing(8);

    name_ = new QLineEdit(QStringLiteral("Solid"), this);
    form->addRow(QStringLiteral("Name"), name_);

    width_ = new QSpinBox(this);
    width_->setRange(1, 16384);
    width_->setValue(compWidth_);
    form->addRow(QStringLiteral("Width"), width_);

    height_ = new QSpinBox(this);
    height_->setRange(1, 16384);
    height_->setValue(compHeight_);
    form->addRow(QStringLiteral("Height"), height_);

    auto* compSize = new QPushButton(QStringLiteral("Make Comp Size"), this);
    connect(compSize, &QPushButton::clicked, this, [this] {
        width_->setValue(compWidth_);
        height_->setValue(compHeight_);
    });
    form->addRow(QString(), compSize);

    swatch_ = new QPushButton(this);
    swatch_->setFixedHeight(24);
    connect(swatch_, &QPushButton::clicked, this, &NewSolidDialog::pickColor);
    form->addRow(QStringLiteral("Colour"), swatch_);
    updateSwatch();

    layout->addLayout(form);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    name_->setFocus();
    name_->selectAll();
}

void NewSolidDialog::pickColor() {
    const QColor picked =
        QColorDialog::getColor(color_, this, QStringLiteral("Solid Colour"));
    if (picked.isValid()) {
        color_ = picked;
        updateSwatch();
    }
}

void NewSolidDialog::updateSwatch() {
    // The button itself is the swatch, rather than a separate preview + picker button.
    swatch_->setStyleSheet(
        QStringLiteral("background: %1; border: 1px solid %2;")
            .arg(color_.name(), kFieldBorder.name()));
    swatch_->setText(color_.name().toUpper());
}

NewSolidDialog::Settings NewSolidDialog::settings() const {
    Settings out;
    out.name = name_->text().trimmed();
    if (out.name.isEmpty()) {
        out.name = QStringLiteral("Solid");
    }
    out.color = color_;
    // Stored as 0 when matching the composition, so it keeps following comp resizes.
    out.width = (width_->value() == compWidth_) ? 0 : width_->value();
    out.height = (height_->value() == compHeight_) ? 0 : height_->value();
    return out;
}

}  // namespace ruby::ui
