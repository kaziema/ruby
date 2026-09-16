#include "ruby/ui/AlignPanel.h"

#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>

#include "ruby/ui/Theme.h"

namespace ruby::ui {

using namespace theme;

namespace {

constexpr int kPad = 10;
constexpr int kRowGap = 8;
constexpr int kBtnW = 30;
constexpr int kBtnH = 24;
constexpr int kBtnGap = 4;
constexpr int kLabelH = 18;
constexpr int kTargetH = 22;

// Six align glyphs, then six distribute ones, in layout order. Drawn from rectangles
// rather than a font, since a diagram reads better than a glyph at this size.
void drawAlignGlyph(QPainter& p, int glyph, const QRect& box, const QColor& ink) {
    const double cx = box.center().x() + 0.5;
    const double cy = box.center().y() + 0.5;
    const bool vertical = glyph >= 3;  // top, vcenter, bottom work on the other axis
    const int edge = glyph % 3;        // 0 near, 1 centre, 2 far

    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(ink);

    // The rule everything lines up against, drawn brighter than the layers.
    const double rule = -6.0 + edge * 6.0;  // -6 near edge, 0 centre, +6 far edge
    if (vertical) {
        p.drawRect(QRectF(cx - 7.0, cy + rule - 0.5, 14.0, 1.0));
    } else {
        p.drawRect(QRectF(cx + rule - 0.5, cy - 7.0, 1.0, 14.0));
    }

    // Two different-sized bars, so which edge is aligned is unambiguous (identical
    // rectangles would look the same left/centre/right).
    QColor bar = ink;
    bar.setAlphaF(ink.alphaF() * 0.62F);
    p.setBrush(bar);

    const double lengths[2] = {9.0, 5.0};
    for (int i = 0; i < 2; ++i) {
        const double len = lengths[i];
        const double off = (i == 0) ? -3.0 : 2.0;  // stacked either side of the middle
        if (vertical) {
            const double y = (edge == 0) ? rule + 1.0 : (edge == 2) ? rule - len - 1.0
                                                                    : rule - len / 2.0;
            p.drawRect(QRectF(cx + off - 1.5, cy + y, 3.0, len));
        } else {
            const double x = (edge == 0) ? rule + 1.0 : (edge == 2) ? rule - len - 1.0
                                                                    : rule - len / 2.0;
            p.drawRect(QRectF(cx + x, cy + off - 1.5, len, 3.0));
        }
    }
    p.restore();
}

void drawDistributeGlyph(QPainter& p, int glyph, const QRect& box, const QColor& ink) {
    const double cx = box.center().x() + 0.5;
    const double cy = box.center().y() + 0.5;
    const bool vertical = glyph >= 3;

    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    // Three evenly spaced bars; the three variants per axis differ only in which edge
    // spacing is measured from, which isn't drawable at this size.
    for (int i = 0; i < 3; ++i) {
        const double o = -6.0 + i * 6.0;
        if (vertical) {
            p.drawRect(QRectF(cx - 6.0, cy + o - 1.0, 12.0, 2.0));
        } else {
            p.drawRect(QRectF(cx + o - 1.0, cy - 6.0, 2.0, 12.0));
        }
    }
    p.restore();
}

}  // namespace

AlignPanel::AlignPanel(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(true);
    setMouseTracking(true);

    using A = Align;
    const struct {
        A align;
        const char* tip;
    } aligns[] = {
        {A::Left, "Align left edges to the composition's left edge"},
        {A::HCenter, "Centre horizontally in the composition"},
        {A::Right, "Align right edges to the composition's right edge"},
        {A::Top, "Align top edges to the composition's top edge"},
        {A::VCenter, "Centre vertically in the composition"},
        {A::Bottom, "Align bottom edges to the composition's bottom edge"},
    };
    for (int i = 0; i < 6; ++i) {
        buttons_.push_back({QRect(), i, false, aligns[i].align, aligns[i].tip});
    }
    const struct {
        A axis;
        const char* tip;
    } spread[] = {
        {A::Left, "Distribute horizontally by left edges"},
        {A::HCenter, "Distribute horizontally by centres"},
        {A::Right, "Distribute horizontally by right edges"},
        {A::Top, "Distribute vertically by top edges"},
        {A::VCenter, "Distribute vertically by centres"},
        {A::Bottom, "Distribute vertically by bottom edges"},
    };
    for (int i = 0; i < 6; ++i) {
        buttons_.push_back({QRect(), i, true, spread[i].axis, spread[i].tip});
    }
    layoutButtons();
}

void AlignPanel::setSelectionCount(int count) {
    count = std::max(0, count);
    if (selectionCount_ == count) {
        return;
    }
    selectionCount_ = count;
    // Falls back to Composition target since aligning a single-item selection to itself
    // is meaningless.
    if (target_ == Target::Selection && selectionCount_ < 2) {
        target_ = Target::Composition;
    }
    update();
}

bool AlignPanel::enabled(const Button& b) const {
    // Distribute needs 3+ (nothing to space with 2). Align needs 1 against composition,
    // 2+ against a selection.
    if (b.distribute) {
        return selectionCount_ >= 3;
    }
    return target_ == Target::Selection ? selectionCount_ >= 2 : selectionCount_ >= 1;
}

void AlignPanel::layoutButtons() {
    // Buttons narrow to fit available width rather than running off the edge; no lower
    // bound, since a minimum would just make them overflow instead.
    const int room = width() - kPad * 2 - kBtnGap * 5;
    const int w = std::max(1, std::min(kBtnW, room / 6));

    int y = kPad + kLabelH + kTargetH + kRowGap;
    for (int i = 0; i < 6; ++i) {
        buttons_[static_cast<std::size_t>(i)].rect =
            QRect(kPad + i * (w + kBtnGap), y, w, kBtnH);
    }
    y += kBtnH + kRowGap + kLabelH;
    for (int i = 6; i < 12; ++i) {
        buttons_[static_cast<std::size_t>(i)].rect =
            QRect(kPad + (i - 6) * (w + kBtnGap), y, w, kBtnH);
    }

    // Dropdown starts after the label's measured width, not a hardcoded guess.
    QFont small = font();
    small.setPixelSize(type::kColumnHeader);
    labelW_ = QFontMetrics(small).horizontalAdvance(QStringLiteral("Align Layers to:")) + 8;
    const int left = kPad + labelW_;
    targetRect_ = QRect(left, kPad + kLabelH - 2, std::max(52, width() - kPad - left),
                        kTargetH);
}

void AlignPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    layoutButtons();
    update();
}

QRect AlignPanel::buttonRect(int index) const {
    return (index >= 0 && index < static_cast<int>(buttons_.size()))
               ? buttons_[static_cast<std::size_t>(index)].rect
               : QRect();
}

int AlignPanel::buttonCount() const { return static_cast<int>(buttons_.size()); }

int AlignPanel::buttonAt(const QPoint& pos) const {
    for (int i = 0; i < static_cast<int>(buttons_.size()); ++i) {
        if (buttons_[static_cast<std::size_t>(i)].rect.contains(pos)) {
            return i;
        }
    }
    return -1;
}

void AlignPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kPanelBody);

    QFont small = font();
    small.setPixelSize(type::kColumnHeader);

    p.setFont(small);
    p.setPen(kTextTertiary);
    p.drawText(QRect(kPad, kPad, labelW_, kLabelH), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("Align Layers to:"));

    // Target dropdown; Composition is the default shown, Selection offered in the menu.
    p.setPen(QPen(kDivider, 1.0));
    p.setBrush(kColumnHeader);
    p.drawRect(targetRect_.adjusted(0, 0, -1, -1));
    p.setBrush(Qt::NoBrush);
    p.setFont(font());
    p.setPen(kTextBody);
    p.drawText(targetRect_.adjusted(7, 0, -18, 0), Qt::AlignVCenter | Qt::AlignLeft,
               target_ == Target::Selection ? QStringLiteral("Selection")
                                            : QStringLiteral("Composition"));
    p.setPen(kTextDim);
    p.drawText(targetRect_.adjusted(0, 0, -7, 0), Qt::AlignVCenter | Qt::AlignRight,
               QStringLiteral("▾"));

    p.setFont(small);
    p.setPen(kTextTertiary);
    p.drawText(QRect(kPad, buttons_[6].rect.top() - kLabelH, 160, kLabelH),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Distribute Layers:"));

    for (int i = 0; i < static_cast<int>(buttons_.size()); ++i) {
        const Button& b = buttons_[static_cast<std::size_t>(i)];
        const bool on = enabled(b);

        if (on && i == hover_) {
            p.fillRect(b.rect, kMenuActive.darker(130));
        }

        const QColor ink = on ? kTextBody : QColor("#4a4a4a");
        if (b.distribute) {
            drawDistributeGlyph(p, b.glyph, b.rect, ink);
        } else {
            drawAlignGlyph(p, b.glyph, b.rect, ink);
        }
    }

    // States why the buttons are greyed out, rather than leaving that to guesswork.
    QString note;
    if (selectionCount_ == 0) {
        note = QStringLiteral("Select a layer with a picture to align it.");
    } else if (selectionCount_ < 3) {
        note = QStringLiteral("Distribute needs three or more layers.");
    }
    if (!note.isEmpty()) {
        p.setFont(small);
        p.setPen(kTextFaint);
        p.drawText(QRect(kPad, buttons_[6].rect.bottom() + kRowGap * 2,
                         width() - kPad * 2, 40),
                   Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, note);
    }
}

void AlignPanel::mousePressEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();

    if (targetRect_.contains(pos)) {
        QMenu menu(this);
        QAction* comp = menu.addAction(QStringLiteral("Composition"));
        comp->setCheckable(true);
        comp->setChecked(target_ == Target::Composition);
        connect(comp, &QAction::triggered, this, [this] {
            target_ = Target::Composition;
            update();
        });

        const bool canSelect = selectionCount_ >= 2;
        QAction* selection = menu.addAction(
            canSelect ? QStringLiteral("Selection")
                      : QStringLiteral("Selection  (needs more than one layer)"));
        selection->setCheckable(true);
        selection->setChecked(target_ == Target::Selection);
        selection->setEnabled(canSelect);
        connect(selection, &QAction::triggered, this, [this] {
            target_ = Target::Selection;
            update();
        });
        menu.exec(e->globalPosition().toPoint());
        return;
    }

    const int hit = buttonAt(pos);
    if (hit < 0 || !enabled(buttons_[static_cast<std::size_t>(hit)])) {
        return;  // a disabled button swallows its click rather than doing nothing loudly
    }
    const Button& b = buttons_[static_cast<std::size_t>(hit)];
    if (b.distribute) {
        // Distribute buttons reuse the Align enum for axis (left/centre/right =
        // horizontal, others vertical) rather than a second enum.
        emit distributeRequested(b.align);
    } else {
        emit alignRequested(b.align, target_);
    }
}

void AlignPanel::mouseMoveEvent(QMouseEvent* e) {
    const int hit = buttonAt(e->position().toPoint());
    if (hit != hover_) {
        hover_ = hit;
        update();
    }
}

void AlignPanel::leaveEvent(QEvent*) {
    if (hover_ != -1) {
        hover_ = -1;
        update();
    }
}

bool AlignPanel::event(QEvent* e) {
    if (e->type() != QEvent::ToolTip) {
        return QWidget::event(e);
    }
    auto* help = static_cast<QHelpEvent*>(e);
    const int hit = buttonAt(help->pos());
    QString text;
    if (hit >= 0) {
        text = QString::fromUtf8(buttons_[static_cast<std::size_t>(hit)].tip);
    } else if (targetRect_.contains(help->pos())) {
        text = QStringLiteral("What layers line up against. Composition means the frame's "
                              "edges and centre.");
    }
    QToolTip::showText(help->globalPos(), text, this);
    e->accept();
    return true;
}

}  // namespace ruby::ui
