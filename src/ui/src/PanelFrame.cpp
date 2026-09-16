#include "ruby/ui/PanelFrame.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QStackedWidget>
#include <QStringList>
#include <QToolTip>
#include <QVBoxLayout>

#include <map>

#include "ruby/ui/Theme.h"

namespace ruby::ui {

using namespace theme;

namespace {

constexpr const char* kTabMime = "application/x-ruby-tab";

// Live frames, by id. An id (not a raw pointer) in the drag's mime data can't dangle if
// the source frame is destroyed mid-drag; worst case is a lookup miss, not a crash.
std::map<quintptr, PanelFrame*>& registry() {
    static std::map<quintptr, PanelFrame*> frames;
    return frames;
}

quintptr nextFrameId() {
    static quintptr next = 1;
    return next++;
}

}  // namespace

// --- Tab strip ---------------------------------------------------------------

class PanelFrame::TabStrip : public QWidget {
public:
    explicit TabStrip(const QStringList& labels, QWidget* parent)
        : QWidget(parent), labels_(labels) {
        setFixedHeight(metrics::kTabStripH);
        setMouseTracking(true);
        setAcceptDrops(true);
        QFont f = font();
        f.setPixelSize(type::kTabLabel);
        setFont(f);
        layoutTabs();
    }

    [[nodiscard]] int current() const { return current_; }
    [[nodiscard]] int count() const { return static_cast<int>(labels_.size()); }
    [[nodiscard]] QString labelAt(int i) const { return labels_.value(i); }
    [[nodiscard]] QRect rectAt(int i) const { return tabRects_.value(i); }

    void setLabels(const QStringList& labels) {
        labels_ = labels;
        if (current_ >= labels_.size()) {
            current_ = labels_.isEmpty() ? 0 : labels_.size() - 1;
        }
        layoutTabs();
        update();
    }

    void setCurrent(int index) {
        if (index < 0 || index >= labels_.size() || index == current_) {
            return;
        }
        current_ = index;
        update();
    }

    void setLabelAt(int index, const QString& label) {
        if (index < 0 || index >= labels_.size()) {
            return;
        }
        labels_[index] = label;
        layoutTabs();
        update();
    }

    void insertLabel(int index, const QString& label) {
        labels_.insert(std::clamp(index, 0, static_cast<int>(labels_.size())), label);
        layoutTabs();
        update();
    }

    QString removeLabel(int index) {
        if (index < 0 || index >= labels_.size()) {
            return {};
        }
        const QString gone = labels_.takeAt(index);
        if (current_ >= labels_.size()) {
            current_ = labels_.isEmpty() ? 0 : static_cast<int>(labels_.size()) - 1;
        }
        layoutTabs();
        update();
        return gone;
    }

    quintptr frameId = 0;
    bool movable = true;
    std::function<void(int)> onActivate;
    std::function<void(quintptr, int, int)> onDropTab;  // source id, source tab, at

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), kTabStrip);

        for (int i = 0; i < labels_.size(); ++i) {
            const QRect r = tabRects_.at(i);
            const bool active = (i == current_);

            if (active) {
                p.fillRect(r, kTabActiveBg);
            } else if (i == hover_) {
                p.fillRect(r, kMenuActive.darker(130));
            }

            // 4px dot, then the label.
            const QColor dot = active ? kTabDotActive : kTabDotInactive;
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(dot);
            p.drawEllipse(QPointF(r.left() + kPadX + 2.0, r.center().y() + 0.5), 2.0, 2.0);
            p.setRenderHint(QPainter::Antialiasing, false);

            p.setPen(active ? kTabActiveText : kTabInactiveText);
            const QRect textRect = r.adjusted(kPadX + kDotW, 0, -kPadX, 0);
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                       QFontMetrics(p.font()).elidedText(labels_.at(i), Qt::ElideRight,
                                                         textRect.width()));
        }

        // Insertion seam for a dragged tab.
        if (dropAt_ >= 0) {
            const int x = dropAt_ < tabRects_.size() ? tabRects_.at(dropAt_).left()
                          : tabRects_.isEmpty()      ? 0
                                                     : tabRects_.last().right();
            p.fillRect(QRect(x - 1, 2, 2, height() - 6), kAccent);
        }

        // Hard rule under the strip.
        p.setPen(kDivider);
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

    void mousePressEvent(QMouseEvent* e) override {
        pressAt_ = e->position().toPoint();
        pressedTab_ = tabAt(pressAt_);

        const int hit = pressedTab_;
        if (hit >= 0 && hit != current_) {
            setCurrent(hit);
            if (onActivate) {
                onActivate(hit);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        const QPoint pos = e->position().toPoint();

        if (movable && pressedTab_ >= 0 && (e->buttons() & Qt::LeftButton) != 0 &&
            (pos - pressAt_).manhattanLength() >= QApplication::startDragDistance()) {
            startDrag(pressedTab_);
            return;
        }

        const int hit = tabAt(pos);
        if (hit != hover_) {
            hover_ = hit;
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override { pressedTab_ = -1; }

    void leaveEvent(QEvent*) override {
        if (hover_ != -1) {
            hover_ = -1;
            update();
        }
    }

    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData() != nullptr && e->mimeData()->hasFormat(kTabMime)) {
            e->acceptProposedAction();
        }
    }

    void dragMoveEvent(QDragMoveEvent* e) override {
        if (e->mimeData() == nullptr || !e->mimeData()->hasFormat(kTabMime)) {
            return;
        }
        const int at = seamAt(e->position().toPoint());
        if (at != dropAt_) {
            dropAt_ = at;
            update();
        }
        e->acceptProposedAction();
    }

    void dragLeaveEvent(QDragLeaveEvent*) override {
        dropAt_ = -1;
        update();
    }

    // Tab width depends on available room, so relayout on resize.
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        layoutTabs();
        update();
    }

    // Tooltip with the full label when a squeezed tab elides it.
    bool event(QEvent* e) override {
        if (e->type() != QEvent::ToolTip) {
            return QWidget::event(e);
        }
        auto* help = static_cast<QHelpEvent*>(e);
        const int hit = tabAt(help->pos());
        const QString label = labels_.value(hit);
        const QRect r = hit >= 0 ? tabRects_.at(hit) : QRect();
        const int room = r.width() - (kPadX + kDotW) - kPadX;
        const bool clipped =
            hit >= 0 && QFontMetrics(font()).horizontalAdvance(label) > room;
        QToolTip::showText(help->globalPos(), clipped ? label : QString(), this);
        e->accept();
        return true;
    }

    void dropEvent(QDropEvent* e) override {
        const int at = dropAt_;
        dropAt_ = -1;
        update();
        if (e->mimeData() == nullptr || !e->mimeData()->hasFormat(kTabMime) || at < 0) {
            return;
        }
        const QByteArray payload = e->mimeData()->data(kTabMime);
        const QList<QByteArray> parts = payload.split(':');
        if (parts.size() != 2) {
            return;
        }
        e->acceptProposedAction();
        if (onDropTab) {
            onDropTab(static_cast<quintptr>(parts.at(0).toULongLong()),
                      parts.at(1).toInt(), at);
        }
    }

private:
    static constexpr int kPadX = 9;
    // Floor width: dot + pads + a couple chars + ellipsis.
    static constexpr int kMinTabW = 46;
    static constexpr int kDotW = 10;

    void startDrag(int index) {
        pressedTab_ = -1;
        if (index < 0 || index >= labels_.size()) {
            return;
        }
        auto* mime = new QMimeData;
        mime->setData(kTabMime, QByteArray::number(static_cast<qulonglong>(frameId)) +
                                    ':' + QByteArray::number(index));

        // Drag badge: the tab drawn as it looks in the strip.
        const QRect r = tabRects_.at(index);
        QPixmap badge(r.size());
        badge.fill(kTabActiveBg);
        {
            QPainter bp(&badge);
            bp.setFont(font());
            bp.setRenderHint(QPainter::Antialiasing, true);
            bp.setPen(Qt::NoPen);
            bp.setBrush(kTabDotActive);
            bp.drawEllipse(QPointF(kPadX + 2.0, badge.height() / 2.0 + 0.5), 2.0, 2.0);
            bp.setRenderHint(QPainter::Antialiasing, false);
            bp.setPen(kTabActiveText);
            const QRect textRect(kPadX + kDotW, 0, badge.width() - kPadX * 2 - kDotW,
                                 badge.height());
            bp.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                        QFontMetrics(bp.font())
                            .elidedText(labels_.at(index), Qt::ElideRight,
                                        textRect.width()));
        }

        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(badge);
        drag->setHotSpot(QPoint(pressAt_.x() - r.left(), badge.height() / 2));
        drag->exec(Qt::MoveAction);
    }

    // Natural width until tabs stop fitting, then they share the strip's width so none
    // ever renders off-edge and unclickable.
    void layoutTabs() {
        const QFontMetrics fm(font());
        tabRects_.clear();
        if (labels_.isEmpty()) {
            return;
        }

        QList<int> natural;
        natural.reserve(labels_.size());
        int total = 0;
        for (const QString& label : labels_) {
            const int w = kPadX + kDotW + fm.horizontalAdvance(label) + kPadX;
            natural.append(w);
            total += w;
        }

        const int avail = std::max(1, width());
        QList<int> widths = natural;

        if (total > avail) {
            // Every tab keeps a floor width; extra space above that is shared out
            // proportional to demand (a flat percentage cut would starve short tabs).
            int floors = 0;
            int wanted = 0;
            for (const int w : natural) {
                floors += std::min(w, kMinTabW);
                wanted += std::max(0, w - kMinTabW);
            }

            if (floors >= avail || wanted <= 0) {
                // Not even room for floors; split evenly so all tabs stay reachable.
                for (int i = 0; i < widths.size(); ++i) {
                    widths[i] = avail / widths.size();
                }
            } else {
                const int slack = avail - floors;
                for (int i = 0; i < widths.size(); ++i) {
                    const int base = std::min(natural.at(i), kMinTabW);
                    const int extra = std::max(0, natural.at(i) - kMinTabW);
                    widths[i] = base + static_cast<int>(
                                           static_cast<qint64>(extra) * slack / wanted);
                }
            }

            // Give integer-division remainder pixels to the last tab.
            int laid = 0;
            for (const int w : widths) {
                laid += w;
            }
            widths.last() += avail - laid;
        }

        int x = 0;
        for (const int w : widths) {
            tabRects_.append(QRect(x, 0, w, metrics::kTabStripH));
            x += w;
        }
    }

    [[nodiscard]] int tabAt(const QPoint& pos) const {
        for (int i = 0; i < tabRects_.size(); ++i) {
            if (tabRects_.at(i).contains(pos)) {
                return i;
            }
        }
        return -1;
    }

    // Seam a drop at this x belongs to; past a tab's midpoint counts as after it.
    [[nodiscard]] int seamAt(const QPoint& pos) const {
        for (int i = 0; i < tabRects_.size(); ++i) {
            const QRect r = tabRects_.at(i);
            if (pos.x() < r.center().x()) {
                return i;
            }
        }
        return static_cast<int>(tabRects_.size());
    }

    QStringList labels_;
    QList<QRect> tabRects_;
    int current_ = 0;
    int hover_ = -1;
    int dropAt_ = -1;
    int pressedTab_ = -1;
    QPoint pressAt_;
};

// --- PanelFrame --------------------------------------------------------------

PanelFrame::PanelFrame(const QStringList& tabs, QWidget* parent) : QWidget(parent) {
    // 1px inset keeps the border painted in paintEvent visible under the strip/page.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    strip_ = new TabStrip(tabs, this);
    stack_ = new QStackedWidget(this);

    layout->addWidget(strip_);
    layout->addWidget(stack_, 1);

    const quintptr id = nextFrameId();
    strip_->frameId = id;
    registry().emplace(id, this);

    strip_->onActivate = [this](int index) {
        // Only switch pages if one exists for this tab (timeline tabs share one page).
        if (index < stack_->count()) {
            stack_->setCurrentIndex(index);
        }
        emit currentChanged(index);
    };

    strip_->onDropTab = [this](quintptr sourceId, int sourceTab, int at) {
        const auto found = registry().find(sourceId);
        if (found == registry().end()) {
            return;  // source gone
        }
        PanelFrame* source = found->second;

        // Reordering in place: removing the tab shifts later seams back by one.
        if (source == this && sourceTab < at) {
            --at;
        }
        DetachedTab moved = source->takeTab(sourceTab);
        if (moved.page == nullptr) {
            return;
        }
        insertTab(at, moved.label, moved.page);
        setCurrentIndex(std::clamp(at, 0, tabCount() - 1));
    };
}

PanelFrame::~PanelFrame() {
    for (auto it = registry().begin(); it != registry().end(); ++it) {
        if (it->second == this) {
            registry().erase(it);
            return;
        }
    }
}

void PanelFrame::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kPanelBody);
    p.setPen(kPanelBorder);
    p.drawRect(rect().adjusted(0, 0, -1, -1));
}

void PanelFrame::addPage(QWidget* page) { stack_->addWidget(page); }

void PanelFrame::setTabs(const QStringList& tabs) { strip_->setLabels(tabs); }

void PanelFrame::setTabsMovable(bool movable) {
    movable_ = movable;
    strip_->movable = movable;
    // Non-page tab strips (e.g. timeline's) must reject drops too.
    strip_->setAcceptDrops(movable);
}

int PanelFrame::tabCount() const { return strip_->count(); }

QString PanelFrame::tabLabel(int index) const { return strip_->labelAt(index); }

void PanelFrame::setTabLabel(int index, const QString& label) {
    if (index < 0 || index >= strip_->count() || strip_->labelAt(index) == label) {
        return;
    }
    strip_->setLabelAt(index, label);
}

PanelFrame* PanelFrame::frameHolding(QWidget* page, int* indexOut) {
    if (page == nullptr) {
        return nullptr;
    }
    for (const auto& [id, frame] : registry()) {
        for (int i = 0; i < frame->stack_->count(); ++i) {
            if (frame->stack_->widget(i) == page) {
                if (indexOut != nullptr) {
                    *indexOut = i;
                }
                return frame;
            }
        }
    }
    return nullptr;
}

int PanelFrame::currentIndex() const { return strip_->current(); }

void PanelFrame::setCurrentIndex(int index) {
    strip_->setCurrent(index);
    if (index >= 0 && index < stack_->count()) {
        stack_->setCurrentIndex(index);
    }
}

PanelFrame::DetachedTab PanelFrame::takeTab(int index) {
    DetachedTab out;
    if (index < 0 || index >= strip_->count() || index >= stack_->count()) {
        return out;
    }
    out.page = stack_->widget(index);
    stack_->removeWidget(out.page);
    out.page->setParent(nullptr);
    out.label = strip_->removeLabel(index);

    setCurrentIndex(std::clamp(strip_->current(), 0, std::max(0, tabCount() - 1)));
    emit tabsChanged();
    return out;
}

QRect PanelFrame::tabRect(int index) const { return strip_->rectAt(index); }

void PanelFrame::insertTab(int index, const QString& label, QWidget* page) {
    if (page == nullptr) {
        return;
    }
    const int at = std::clamp(index, 0, tabCount());
    stack_->insertWidget(at, page);
    strip_->insertLabel(at, label);
    emit tabsChanged();
}

}  // namespace ruby::ui
