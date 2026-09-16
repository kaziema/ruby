#include "ruby/ui/PooledMediaPanel.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>

#include "ruby/ui/Theme.h"

namespace ruby::ui {

using namespace theme;

namespace {

constexpr int kTypeW = 52;
constexpr int kDurW = 48;
constexpr int kAddedW = 74;
constexpr int kEdgePad = 8;
constexpr int kFolderRows = 1;  // the single "All Media" root

QString formatDuration(double seconds) {
    if (seconds <= 0.0) {
        return QStringLiteral("—");
    }
    const int total = static_cast<int>(seconds);
    return QStringLiteral("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Relative for recent items, then a plain date once "N days ago" stops being useful.
QString formatAdded(std::int64_t epochSeconds) {
    if (epochSeconds <= 0) {
        return QStringLiteral("—");
    }
    const QDateTime when = QDateTime::fromSecsSinceEpoch(epochSeconds);
    const qint64 days = when.daysTo(QDateTime::currentDateTime());
    if (days <= 0) {
        return QStringLiteral("today");
    }
    if (days == 1) {
        return QStringLiteral("yesterday");
    }
    if (days < 30) {
        return QStringLiteral("%1d ago").arg(days);
    }
    return when.toString(QStringLiteral("d MMM yy"));
}

// Same palette the project panel uses, so a clip looks like the same clip in both lists.
QColor swatchFor(core::MediaKind kind) {
    switch (kind) {
        case core::MediaKind::Video: return kLabelAqua.stripe;
        case core::MediaKind::Audio: return kLabelGreen.stripe;
        case core::MediaKind::Image: return kLabelLavender.stripe;
        case core::MediaKind::Unknown: return kLabelGray.stripe;
    }
    return kLabelGray.stripe;
}

const char* typeFor(core::MediaKind kind) {
    switch (kind) {
        case core::MediaKind::Video: return "Video";
        case core::MediaKind::Audio: return "Audio";
        case core::MediaKind::Image: return "Still";
        case core::MediaKind::Unknown: return "—";
    }
    return "—";
}

}  // namespace

PooledMediaPanel::PooledMediaPanel(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(true);
}

void PooledMediaPanel::setPool(const io::MediaPool* pool) {
    pool_ = pool;
    refresh();
}

void PooledMediaPanel::refresh() {
    rebuild();
    update();
}

void PooledMediaPanel::rebuild() {
    rows_.clear();
    if (pool_ == nullptr) {
        return;
    }
    for (const io::PooledItem& item : pool_->items()) {
        Row row;
        row.name = QString::fromStdString(item.name);
        row.type = QString::fromUtf8(typeFor(item.kind));
        row.duration = formatDuration(item.duration);
        row.added = formatAdded(item.firstSeen);
        row.swatch = swatchFor(item.kind);
        // Checked every refresh rather than cached, since files can move while the app
        // is open.
        row.missing = !QFileInfo::exists(QString::fromStdString(item.path));
        rows_.push_back(std::move(row));
    }
    if (selected_ >= static_cast<int>(rows_.size())) {
        selected_ = -1;
    }
}

int PooledMediaPanel::rowAt(int y) const {
    const int listY = metrics::kColumnHeaderH + kFolderRows * metrics::kProjectRowH;
    if (y < listY) {
        return -1;
    }
    const int index = (y - listY) / metrics::kProjectRowH;
    return index < static_cast<int>(rows_.size()) ? index : -1;
}

void PooledMediaPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kPanelBody);

    const int nameW = width() - kTypeW - kDurW - kAddedW - kEdgePad;

    // Column header.
    p.fillRect(QRect(0, 0, width(), metrics::kColumnHeaderH), kColumnHeader);
    QFont small = font();
    small.setPixelSize(type::kColumnHeader);
    p.setFont(small);
    p.setPen(kColumnHeaderText);
    p.drawText(QRect(kEdgePad + 20, 0, nameW, metrics::kColumnHeaderH),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Name"));
    p.drawText(QRect(nameW, 0, kTypeW, metrics::kColumnHeaderH),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Type"));
    p.drawText(QRect(nameW + kTypeW, 0, kDurW, metrics::kColumnHeaderH),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Dur"));
    p.drawText(QRect(nameW + kTypeW + kDurW, 0, kAddedW, metrics::kColumnHeaderH),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Added"));

    const int footerY = height() - metrics::kProjectFooterH;

    if (pool_ == nullptr || pool_->empty()) {
        p.setFont(font());
        p.setPen(kTextFaint);
        p.drawText(QRect(kEdgePad, metrics::kColumnHeaderH, width() - kEdgePad * 2,
                         footerY - metrics::kColumnHeaderH),
                   Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("Import media into a project to begin pooling media!"));
        p.fillRect(QRect(0, footerY, width(), metrics::kProjectFooterH), kColumnHeader);
        return;
    }

    // The folder row: one root for now, always open.
    p.setFont(font());
    p.fillRect(QRect(0, metrics::kColumnHeaderH, width(), metrics::kProjectRowH),
               kSubToolbar);
    p.setPen(kTextDim);
    p.drawText(QRect(kEdgePad - 2, metrics::kColumnHeaderH, 10, metrics::kProjectRowH),
               Qt::AlignCenter, folderOpen_ ? QStringLiteral("▾") : QStringLiteral("▸"));
    p.fillRect(QRect(kEdgePad + 9,
                     metrics::kColumnHeaderH + (metrics::kProjectRowH - 10) / 2, 13, 10),
               kLabelGray.stripe);
    p.setPen(kFieldBorder);
    p.drawRect(QRect(kEdgePad + 9,
                     metrics::kColumnHeaderH + (metrics::kProjectRowH - 10) / 2, 13, 10));
    p.setPen(kTextBody);
    p.drawText(QRect(kEdgePad + 28, metrics::kColumnHeaderH, nameW, metrics::kProjectRowH),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("All Media"));

    const int listY = metrics::kColumnHeaderH + kFolderRows * metrics::kProjectRowH;
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const int y = listY + i * metrics::kProjectRowH;
        if (y + metrics::kProjectRowH > footerY) {
            break;  // no scrolling yet, same as the project panel
        }
        const Row& row = rows_[static_cast<std::size_t>(i)];

        const QColor background = (i == selected_) ? kRowSelected
                                 : (i % 2 == 0)    ? kSubToolbar
                                                   : kPanelBody;
        p.fillRect(QRect(0, y, width(), metrics::kProjectRowH), background);

        // Indented under the folder.
        const int swatchX = kEdgePad + 21;
        p.fillRect(QRect(swatchX, y + (metrics::kProjectRowH - 10) / 2, 13, 10),
                   row.missing ? kLabelGray.stripe : row.swatch);
        p.setPen(kFieldBorder);
        p.drawRect(QRect(swatchX, y + (metrics::kProjectRowH - 10) / 2, 13, 10));

        // Missing files stay listed (marked, not dropped) — this is a record of
        // everything ever imported.
        p.setPen(row.missing            ? kTextFaint
                 : i == selected_       ? kTextSelectedLayer
                                        : kTextBody);
        const int textX = swatchX + 19;
        p.drawText(QRect(textX, y, nameW - textX, metrics::kProjectRowH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(font()).elidedText(row.name, Qt::ElideMiddle,
                                                   nameW - textX - 2));

        p.setFont(numericFont(type::kMeta));
        p.setPen(row.missing ? kTextFaint : kTextDim);
        p.drawText(QRect(nameW, y, kTypeW, metrics::kProjectRowH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   row.missing ? QStringLiteral("missing") : row.type);
        p.drawText(QRect(nameW + kTypeW, y, kDurW, metrics::kProjectRowH),
                   Qt::AlignVCenter | Qt::AlignLeft, row.duration);
        p.drawText(QRect(nameW + kTypeW + kDurW, y, kAddedW, metrics::kProjectRowH),
                   Qt::AlignVCenter | Qt::AlignLeft, row.added);
        p.setFont(font());
    }

    p.fillRect(QRect(0, footerY, width(), metrics::kProjectFooterH), kColumnHeader);
    p.setFont(numericFont(type::kMeta));
    p.setPen(kTextDim);
    p.drawText(QRect(kEdgePad, footerY, width(), metrics::kProjectFooterH),
               Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("%1 pooled").arg(rows_.size()));
}

void PooledMediaPanel::mousePressEvent(QMouseEvent* e) {
    const int row = rowAt(e->position().toPoint().y());
    if (row != selected_) {
        selected_ = row;
        update();
    }
}

}  // namespace ruby::ui
