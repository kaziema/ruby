#include "ruby/ui/ProjectPanel.h"

#include <QFileInfo>
#include <QApplication>
#include <QDrag>
#include <QLineEdit>
#include <QMimeData>
#include <QPixmap>
#include <QShortcut>
#include <QMouseEvent>
#include <QToolTip>
#include <QHelpEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QPainter>
#include <QVBoxLayout>

#include "ruby/ui/Theme.h"

namespace ruby::ui {

using namespace theme;

namespace {

constexpr int kSearchH = 26;
// Wide enough for "Composition" / "Video + Audio", the longest values.
constexpr int kTypeW = 84;
constexpr int kDurW = 48;
constexpr int kEdgePad = 8;

QString formatDuration(double seconds) {
    if (seconds <= 0.0) {
        return QStringLiteral("—");
    }
    const int total = static_cast<int>(seconds);
    return QStringLiteral("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString formatSize(qint64 bytes) {
    if (bytes <= 0) {
        return QString();
    }
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) {
        return QStringLiteral("%1 GB").arg(gb, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0),
                                       0, 'f', 0);
}

}  // namespace

ProjectPanel::ProjectPanel(QWidget* parent) : QWidget(parent) {
    setAcceptDrops(true);
    setMouseTracking(true);
    QFont f = font();
    f.setPixelSize(type::kRowLabel);
    setFont(f);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kEdgePad, 5, kEdgePad, 0);
    layout->setSpacing(0);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Search"));
    // Click focus only: as the first focusable widget it would otherwise steal focus at
    // launch and eat the spacebar before the play shortcut sees it.
    search_->setFocusPolicy(Qt::ClickFocus);
    search_->setFixedHeight(kSearchH - 8);
    search_->setStyleSheet(
        QStringLiteral("QLineEdit { background: %1; border: 1px solid %2; color: %3; "
                       "padding: 1px 6px; }")
            .arg(kFieldBg.name(), kFieldBorder.name(), kTextBody.name()));
    layout->addWidget(search_);
    layout->addStretch();

    // Escape returns focus to the window.
    auto* leave = new QShortcut(QKeySequence(Qt::Key_Escape), search_);
    leave->setContext(Qt::WidgetShortcut);
    connect(leave, &QShortcut::activated, this, [this] {
        search_->clearFocus();
        if (window() != nullptr) {
            window()->setFocus();
        }
    });

    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filter_ = text;
        rebuild();
        update();
    });
}

void ProjectPanel::setProject(const core::Project* project) {
    project_ = project;
    refresh();
}

void ProjectPanel::refresh() {
    rebuild();
    update();
}

void ProjectPanel::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData() != nullptr && e->mimeData()->hasFormat(mediaMimeType())) {
        e->acceptProposedAction();
    }
}

void ProjectPanel::dragMoveEvent(QDragMoveEvent* e) {
    // Only the New Composition button accepts a drop; media dropped elsewhere in its
    // own panel means nothing.
    const bool over = newCompRect_.contains(e->position().toPoint());
    if (over != dropOnNewComp_) {
        dropOnNewComp_ = over;
        update();
    }
    if (over) {
        e->acceptProposedAction();
    } else {
        e->ignore();
    }
}

void ProjectPanel::dragLeaveEvent(QDragLeaveEvent*) {
    dropOnNewComp_ = false;
    update();
}

void ProjectPanel::dropEvent(QDropEvent* e) {
    const bool over = newCompRect_.contains(e->position().toPoint());
    dropOnNewComp_ = false;
    update();
    if (!over || e->mimeData() == nullptr) {
        return;
    }
    const auto id = static_cast<core::MediaId>(
        e->mimeData()->data(mediaMimeType()).toULongLong());
    e->acceptProposedAction();
    emit compositionFromMediaRequested(id);
}

void ProjectPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    layoutFooter();
    update();
}

void ProjectPanel::rebuild() {
    rows_.clear();
    totalBytes_ = 0;
    if (project_ == nullptr) {
        return;
    }

    const auto matches = [this](const QString& name) {
        return filter_.isEmpty() || name.contains(filter_, Qt::CaseInsensitive);
    };

    // Compositions above media, the way every editor lists them.
    for (const core::Composition& comp : project_->compositions()) {
        const QString name = QString::fromStdString(comp.name);
        if (!matches(name)) {
            continue;
        }
        rows_.push_back({true, comp.id, name, QStringLiteral("Composition"),
                         formatDuration(comp.duration), kLabelAqua.stripe, 0,
                         QStringLiteral("%1 x %2  ·  %3 fps  ·  %4 layers")
                             .arg(comp.width)
                             .arg(comp.height)
                             .arg(comp.fps, 0, 'g', 5)
                             .arg(comp.layers.size())});
    }

    for (const core::MediaItem& item : project_->media()) {
        const QString name = QString::fromStdString(item.name);
        if (!matches(name)) {
            continue;
        }

        // Media kind for the Type column (not resolution class, not an importer name).
        QString kind = QStringLiteral("Video");
        QColor swatch = kLabelAqua.stripe;
        switch (item.kind) {
            case core::MediaKind::Video:
                kind = item.hasAudio ? QStringLiteral("Video + Audio")
                                     : QStringLiteral("Video");
                break;
            case core::MediaKind::Audio:
                kind = QStringLiteral("Audio");
                swatch = kLabelGreen.stripe;
                break;
            case core::MediaKind::Image:
                kind = QStringLiteral("Still");
                swatch = kLabelLavender.stripe;
                break;
            case core::MediaKind::Unknown:
                kind = QStringLiteral("Unknown");
                swatch = kLabelGray.stripe;
                break;
        }

        const qint64 bytes =
            QFileInfo(QString::fromStdString(item.path)).size();
        totalBytes_ += bytes;

        QString detail;
        if (item.width > 0 && item.height > 0) {
            detail = QStringLiteral("%1 x %2").arg(item.width).arg(item.height);
            if (item.fps > 0.0) {
                detail += QStringLiteral("  ·  %1 fps").arg(item.fps, 0, 'g', 5);
            }
            detail += QStringLiteral("\n");
        }
        detail += QString::fromStdString(item.path);

        rows_.push_back({false, item.id, name, kind, formatDuration(item.duration),
                         swatch, bytes, detail});
    }

    if (selected_ >= static_cast<int>(rows_.size())) {
        selected_ = -1;
    }
}

// Footer layout in one place: the count starts where the buttons end, by construction.
void ProjectPanel::layoutFooter() {
    const int y = height() - metrics::kProjectFooterH;
    const int size = metrics::kProjectFooterH - 6;

    newCompRect_ = QRect(kEdgePad, y + 3, size, size);
    deleteRect_ = QRect(newCompRect_.right() + 7, y + 3, size, size);

    // Count then size share the remaining width; size stays right-aligned for a glance.
    const int textLeft = deleteRect_.right() + 12;
    const int available = width() - textLeft - kEdgePad;
    countRect_ = QRect(textLeft, y, available / 2, metrics::kProjectFooterH);
    sizeRect_ = QRect(textLeft + available / 2, y, available - available / 2,
                      metrics::kProjectFooterH);
}

int ProjectPanel::rowAt(int y) const {
    const int top = kSearchH + metrics::kColumnHeaderH;
    // Must match where painting stops, or hidden rows below the footer stay clickable.
    const int bottom = height() - metrics::kProjectFooterH;
    if (y < top || y >= bottom) {
        return -1;
    }
    const int index = (y - top) / metrics::kProjectRowH;
    return (index >= 0 && index < static_cast<int>(rows_.size())) ? index : -1;
}

void ProjectPanel::paintEvent(QPaintEvent*) {
    // Recomputed here (not just on resize) so layout and painting can't disagree.
    layoutFooter();

    QPainter p(this);
    p.fillRect(rect(), kPanelBody);

    const int headerY = kSearchH;
    const int listY = headerY + metrics::kColumnHeaderH;
    const int nameW = width() - kTypeW - kDurW - kEdgePad;

    // Column header.
    p.fillRect(QRect(0, headerY, width(), metrics::kColumnHeaderH), kColumnHeader);
    QFont small = font();
    small.setPixelSize(type::kColumnHeader);
    p.setFont(small);
    p.setPen(kColumnHeaderText);
    p.drawText(QRect(kEdgePad + 20, headerY, nameW, metrics::kColumnHeaderH),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Name"));
    p.drawText(QRect(nameW, headerY, kTypeW, metrics::kColumnHeaderH),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Type"));
    p.drawText(QRect(nameW + kTypeW, headerY, kDurW, metrics::kColumnHeaderH),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Dur"));

    const int footerY = height() - metrics::kProjectFooterH;

    if (rows_.empty()) {
        p.setFont(font());
        p.setPen(kTextFaint);
        p.drawText(QRect(0, listY, width(), footerY - listY), Qt::AlignCenter,
                   project_ == nullptr || project_->media().empty()
                       ? QStringLiteral("File > Import Media\nto get started")
                       : QStringLiteral("nothing matches"));
    }

    p.setFont(font());
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const int y = listY + i * metrics::kProjectRowH;
        if (y + metrics::kProjectRowH > footerY) {
            break;  // no scrolling yet; the pool is small
        }
        const Row& row = rows_[static_cast<std::size_t>(i)];

        const QColor background = (i == selected_) ? kRowSelected
                                 : (i % 2 == 0)    ? kSubToolbar
                                                   : kPanelBody;
        p.fillRect(QRect(0, y, width(), metrics::kProjectRowH), background);

        // Twirl slot, then the swatch. Compositions can hold layers; media cannot.
        if (row.isComposition) {
            p.setPen(kTextDim);
            p.drawText(QRect(kEdgePad - 2, y, 10, metrics::kProjectRowH), Qt::AlignCenter,
                       QStringLiteral("▸"));
        }
        p.fillRect(QRect(kEdgePad + 9, y + (metrics::kProjectRowH - 10) / 2, 13, 10),
                   row.swatch);
        p.setPen(kFieldBorder);
        p.drawRect(QRect(kEdgePad + 9, y + (metrics::kProjectRowH - 10) / 2, 13, 10));

        p.setPen(i == selected_ ? kTextSelectedLayer : kTextBody);
        p.drawText(QRect(kEdgePad + 28, y, nameW - kEdgePad - 28, metrics::kProjectRowH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(font()).elidedText(row.name, Qt::ElideMiddle,
                                                   nameW - kEdgePad - 30));

        p.setFont(numericFont(type::kMeta));
        p.setPen(kTextDim);
        p.drawText(QRect(nameW, y, kTypeW, metrics::kProjectRowH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(numericFont(type::kMeta))
                       .elidedText(row.type, Qt::ElideRight, kTypeW - 4));
        p.drawText(QRect(nameW + kTypeW, y, kDurW, metrics::kProjectRowH),
                   Qt::AlignVCenter | Qt::AlignLeft, row.duration);
        p.setFont(font());
    }

    // Footer: how much is in here, and how heavy it is.
    p.fillRect(QRect(0, footerY, width(), metrics::kProjectFooterH), kColumnHeader);
    p.setFont(numericFont(type::kMeta));
    p.setPen(kTextDim);
    p.drawText(countRect_, Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("%1 item%2")
                   .arg(rows_.size())
                   .arg(rows_.size() == 1 ? QString() : QStringLiteral("s")));
    p.drawText(sizeRect_, Qt::AlignVCenter | Qt::AlignRight, formatSize(totalBytes_));

    // New Composition, then Delete.
    p.setRenderHint(QPainter::Antialiasing, true);

    if (dropOnNewComp_) {
        // Lit while footage is hovering over it, so the drop target is visible.
        p.fillRect(newCompRect_.adjusted(-2, -2, 2, 2), kAccent);
    } else if (hoverButton_ == 0) {
        p.fillRect(newCompRect_.adjusted(-2, -2, 2, 2), kMenuActive);
    }
    p.setPen(QPen(dropOnNewComp_ ? QColor("#12212e") : kTextTertiary, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(newCompRect_.adjusted(1, 3, -1, -3));

    if (hoverButton_ == 1) {
        p.fillRect(deleteRect_.adjusted(-2, -2, 2, 2), kMenuActive);
    }
    const bool canDelete = selected_ >= 0;
    p.setPen(QPen(canDelete ? kTextTertiary : kTextFaint, 1.0));
    {
        // A bin: lid, body, and two lines down it.
        const QRect r = deleteRect_.adjusted(2, 2, -2, -2);
        p.drawLine(r.left(), r.top() + 3, r.right(), r.top() + 3);
        p.drawLine(r.left() + 3, r.top() + 3, r.left() + 3, r.bottom());
        p.drawLine(r.right() - 3, r.top() + 3, r.right() - 3, r.bottom());
        p.drawLine(r.left() + 3, r.bottom(), r.right() - 3, r.bottom());
        p.drawLine(r.center().x() - 2, r.top(), r.center().x() + 2, r.top());
    }
    p.setRenderHint(QPainter::Antialiasing, false);
}

const char* ProjectPanel::mediaMimeType() { return "application/x-ruby-media"; }

bool ProjectPanel::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(e);
        if (newCompRect_.contains(help->pos())) {
            QToolTip::showText(help->globalPos(),
                               QStringLiteral("New Composition — or drop a clip here to "
                                              "make one that matches it"),
                               this);
            return true;
        }
        if (deleteRect_.contains(help->pos())) {
            QToolTip::showText(help->globalPos(),
                               QStringLiteral("Delete the selected item"), this);
            return true;
        }
        const int row = rowAt(help->pos().y());
        if (row < 0) {
            QToolTip::hideText();
            e->ignore();
            return true;
        }
        const Row& hit = rows_[static_cast<std::size_t>(row)];
        // Name first: the column elides it, so this may be the only full readout.
        QToolTip::showText(help->globalPos(),
                           hit.detail.isEmpty()
                               ? hit.name
                               : QStringLiteral("%1\n%2").arg(hit.name, hit.detail),
                           this);
        return true;
    }
    return QWidget::event(e);
}

void ProjectPanel::mousePressEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();

    if (newCompRect_.contains(pos)) {
        emit newCompositionRequested();
        return;
    }
    if (deleteRect_.contains(pos)) {
        if (selected_ >= 0) {
            const Row& row = rows_[static_cast<std::size_t>(selected_)];
            emit deleteRequested(row.isComposition, row.id);
        }
        return;
    }

    const int row = rowAt(pos.y());
    if (row != selected_) {
        selected_ = row;
        update();
    }
    pressAt_ = pos;
    // Only media can be dragged out; dropping a composition on its own timeline is
    // undefined.
    maybeDragging_ = row >= 0 && !rows_[static_cast<std::size_t>(row)].isComposition;
}

void ProjectPanel::mouseMoveEvent(QMouseEvent* e) {
    const QPoint at = e->position().toPoint();
    const int button = newCompRect_.contains(at) ? 0 : (deleteRect_.contains(at) ? 1 : -1);
    if (button != hoverButton_) {
        hoverButton_ = button;
        update();
    }

    if (!maybeDragging_ || (e->buttons() & Qt::LeftButton) == 0) {
        return;
    }
    // Qt's own threshold, so a slightly shaky click does not become a drag.
    if ((e->position().toPoint() - pressAt_).manhattanLength() <
        QApplication::startDragDistance()) {
        return;
    }
    if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) {
        return;
    }
    const Row& row = rows_[static_cast<std::size_t>(selected_)];
    maybeDragging_ = false;

    auto* data = new QMimeData;
    data->setData(mediaMimeType(), QByteArray::number(qulonglong(row.id)));
    data->setText(row.name);  // so dropping elsewhere at least says what it was

    auto* drag = new QDrag(this);
    drag->setMimeData(data);

    // A small label under the cursor, so it is obvious what is being carried.
    const QFontMetrics fm(font());
    const int w = fm.horizontalAdvance(row.name) + 18;
    QPixmap badge(w, metrics::kProjectRowH);
    badge.fill(Qt::transparent);
    {
        QPainter p(&badge);
        p.fillRect(badge.rect(), kRowSelected);
        p.setPen(kFieldBorder);
        p.drawRect(badge.rect().adjusted(0, 0, -1, -1));
        p.fillRect(QRect(5, (badge.height() - 8) / 2, 5, 8), row.swatch);
        p.setFont(font());
        p.setPen(kTextSelectedLayer);
        p.drawText(badge.rect().adjusted(14, 0, -4, 0),
                   Qt::AlignVCenter | Qt::AlignLeft, row.name);
    }
    drag->setPixmap(badge);
    drag->setHotSpot(QPoint(10, badge.height() / 2));
    drag->exec(Qt::CopyAction);
}

void ProjectPanel::mouseDoubleClickEvent(QMouseEvent* e) {
    const int index = rowAt(e->position().toPoint().y());
    if (index < 0) {
        return;
    }
    const Row& row = rows_[static_cast<std::size_t>(index)];
    if (row.isComposition) {
        emit compositionActivated(row.id);
    } else {
        emit mediaActivated(row.id);
    }
}

}  // namespace ruby::ui
