#pragma once

#include <QStringList>
#include <QWidget>

class QStackedWidget;

namespace ruby::ui {

// A panel group: a 26px tab strip over a content area. Base layout unit — tabs are how
// panels group in this design.
//
// Tabs drag between frames; a frame owns whatever tabs it currently holds. No free-form
// docking (AE/Premiere's five-zone overlay) — drop targets are just the visible tab strip.
class PanelFrame : public QWidget {
    Q_OBJECT

public:
    explicit PanelFrame(const QStringList& tabs, QWidget* parent = nullptr);
    ~PanelFrame() override;

    // Content is parallel to the tab list: index N belongs to tab N.
    void addPage(QWidget* page);

    // Replaces tab labels without touching pages. Used when tabs describe content on a
    // single shared page (e.g. the timeline's open compositions) rather than select pages.
    void setTabs(const QStringList& tabs);

    // Whether this frame's tabs can be dragged out. False when tabs aren't pages (e.g.
    // the timeline's tabs name compositions on one shared page, not draggable panels).
    void setTabsMovable(bool movable);
    [[nodiscard]] bool tabsMovable() const noexcept { return movable_; }

    [[nodiscard]] int tabCount() const;
    [[nodiscard]] QString tabLabel(int index) const;

    // Tab's rect in strip coordinates. Public so callers can check actual layout rather
    // than assume natural width.
    [[nodiscard]] QRect tabRect(int index) const;
    void setTabLabel(int index, const QString& label);

    // Which frame currently holds `page`, and at what index; null if none. A tab's home
    // isn't fixed, so callers can't assume a page is still where it started.
    [[nodiscard]] static PanelFrame* frameHolding(QWidget* page, int* indexOut = nullptr);
    [[nodiscard]] int currentIndex() const;
    void setCurrentIndex(int index);

    // Removes a tab and its page and hands both back; page is reparented to nobody
    // (caller owns it until reinserted).
    struct DetachedTab {
        QString label;
        QWidget* page = nullptr;
    };
    [[nodiscard]] DetachedTab takeTab(int index);
    void insertTab(int index, const QString& label, QWidget* page);

signals:
    void currentChanged(int index);

    // A tab arrived or left (used to hide emptied frames, track moved panels).
    void tabsChanged();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    class TabStrip;

    TabStrip* strip_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    bool movable_ = true;
};

}  // namespace ruby::ui
