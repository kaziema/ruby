#include "ruby/ui/EffectsPanel.h"

#include <QApplication>
#include <QDrag>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QVBoxLayout>

#include <map>

#include "ruby/engine/EffectRegistry.h"
#include "ruby/ui/Theme.h"

namespace ruby::ui {

using namespace theme;

namespace {

constexpr int kSearchH = 26;
constexpr int kRowH = 22;
constexpr int kEdgePad = 8;

}  // namespace

EffectsPanel::EffectsPanel(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(true);
    setMouseTracking(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Search"));
    search_->setFixedHeight(kSearchH);
    // Click focus only: grabbing focus at launch would eat the spacebar before playback.
    search_->setFocusPolicy(Qt::ClickFocus);
    layout->addWidget(search_);
    layout->addStretch(1);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filter_ = text.trimmed();
        rebuild();
        update();
    });

    rebuild();
}

const char* EffectsPanel::effectMimeType() { return "application/x-ruby-effect"; }

void EffectsPanel::setTab(Tab tab) {
    if (tab_ == tab) {
        return;
    }
    tab_ = tab;
    selected_ = -1;
    rebuild();
    update();
}

void EffectsPanel::rebuild() {
    rows_.clear();
    if (tab_ != Tab::Effects) {
        return;  // presets and colour correction have nothing behind them yet
    }

    // Grouped by category normally; flat while searching, since headings are noise then.
    const bool searching = !filter_.isEmpty();
    std::map<QString, std::vector<Row>> grouped;

    for (const engine::EffectDef& def : engine::EffectRegistry::instance().all()) {
        const QString name = QString::fromStdString(def.schema.display_name);
        const QString category =
            QString::fromStdString(engine::effectCategory(def.schema.id));

        // Matches category too, so "blur" finds everything under Blur, not just name
        // matches.
        if (searching && !name.contains(filter_, Qt::CaseInsensitive) &&
            !category.contains(filter_, Qt::CaseInsensitive)) {
            continue;
        }
        grouped[category].push_back({false, name, def.schema.id});
    }

    for (auto& [category, effects] : grouped) {
        if (!searching) {
            rows_.push_back({true, category, {}});
        }
        for (Row& row : effects) {
            rows_.push_back(std::move(row));
        }
    }
    if (selected_ >= static_cast<int>(rows_.size())) {
        selected_ = -1;
    }
}

int EffectsPanel::rowAt(int y) const {
    // Must match where painting stops, or rows past the visible area stay interactive.
    if (y < kSearchH || y >= height()) {
        return -1;
    }
    const int index = (y - kSearchH) / kRowH;
    return index < static_cast<int>(rows_.size()) ? index : -1;
}

void EffectsPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kPanelBody);

    if (rows_.empty()) {
        p.setPen(kTextFaint);
        p.setFont(font());
        const QRect box(kEdgePad, kSearchH, width() - kEdgePad * 2, height() - kSearchH);

        // Distinct message per empty reason ("no effects" vs. "search found nothing").
        QString message;
        switch (tab_) {
            case Tab::Effects:
                message = filter_.isEmpty()
                              ? QStringLiteral("No effects registered.")
                              : QStringLiteral("No effect matches that.");
                break;
            case Tab::Presets:
                message = QStringLiteral("Presets arrive with the preset system. This is "
                                         "where they will live.");
                break;
            case Tab::ColorCorrection:
                message = QStringLiteral("Colour correction presets arrive with the "
                                         "preset system.");
                break;
        }
        p.drawText(box, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, message);
        return;
    }

    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const Row& row = rows_[static_cast<std::size_t>(i)];
        const int y = kSearchH + i * kRowH;
        if (y > height()) {
            break;
        }

        if (row.isCategory) {
            p.fillRect(QRect(0, y, width(), kRowH), kColumnHeader);
            QFont small = font();
            small.setPixelSize(type::kColumnHeader);
            p.setFont(small);
            p.setPen(kColumnHeaderText);
            p.drawText(QRect(kEdgePad, y, width(), kRowH),
                       Qt::AlignVCenter | Qt::AlignLeft, row.label);
            continue;
        }

        if (i == selected_) {
            p.fillRect(QRect(0, y, width(), kRowH), kRowSelected);
        }
        // Same fx marker colour the timeline/inspector use for an effect.
        p.fillRect(QRect(kEdgePad + 4, y + kRowH / 2 - 4, 8, 8), kExpressionText);

        p.setFont(font());
        p.setPen(i == selected_ ? kTextSelectedLayer : kTextBody);
        p.drawText(QRect(kEdgePad + 20, y, width() - kEdgePad - 20, kRowH),
                   Qt::AlignVCenter | Qt::AlignLeft, row.label);
    }
}

void EffectsPanel::mousePressEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    const int row = rowAt(pos.y());
    if (row != selected_) {
        selected_ = row;
        update();
    }
    pressAt_ = pos;
    maybeDragging_ =
        row >= 0 && !rows_[static_cast<std::size_t>(row)].isCategory;
}

void EffectsPanel::mouseMoveEvent(QMouseEvent* e) {
    if (!maybeDragging_ || (e->buttons() & Qt::LeftButton) == 0 || selected_ < 0) {
        return;
    }
    if ((e->position().toPoint() - pressAt_).manhattanLength() <
        QApplication::startDragDistance()) {
        return;
    }
    const Row& row = rows_[static_cast<std::size_t>(selected_)];

    auto* mime = new QMimeData;
    mime->setData(effectMimeType(), QByteArray(row.effectId.c_str()));

    // Badge under the cursor so the drag shows its payload.
    const QFontMetrics fm(font());
    const int w = fm.horizontalAdvance(row.label) + 34;
    QPixmap badge(w, kRowH);
    badge.fill(kPanelBody);
    {
        QPainter bp(&badge);
        bp.fillRect(QRect(6, kRowH / 2 - 4, 8, 8), kExpressionText);
        bp.setFont(font());
        bp.setPen(kTextBody);
        bp.drawText(QRect(22, 0, w - 22, kRowH), Qt::AlignVCenter | Qt::AlignLeft,
                    row.label);
    }

    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(badge);
    drag->setHotSpot(QPoint(12, kRowH / 2));
    drag->exec(Qt::CopyAction);
    maybeDragging_ = false;
}

void EffectsPanel::mouseDoubleClickEvent(QMouseEvent* e) {
    const int row = rowAt(e->position().toPoint().y());
    if (row < 0 || rows_[static_cast<std::size_t>(row)].isCategory) {
        return;
    }
    emit effectActivated(rows_[static_cast<std::size_t>(row)].effectId);
}

void EffectsPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    update();
}

}  // namespace ruby::ui
