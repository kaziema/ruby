#pragma once

#include <QString>
#include <QWidget>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QScrollBar;
class QSlider;

#include <map>
#include <set>

#include "ruby/core/Document.h"
#include "ruby/media/PeakCache.h"

class QMimeData;

namespace ruby::ui {

// Identifies one keyframe. Positional until retiming needs a real id.
struct KeyRef {
    core::LayerId layer = 0;
    int effect = -1;  // -1 when the property belongs to the layer itself
    int property = -1;
    int index = -1;

    [[nodiscard]] bool operator==(const KeyRef& other) const noexcept {
        return layer == other.layer && effect == other.effect &&
               property == other.property && index == other.index;
    }
};

// Custom-painted timeline (fixed row heights, shared time axis, keyframe diamonds) —
// doesn't map onto QTableView. Reads a core::Composition and owns none of it.
class TimelineView : public QWidget {
    Q_OBJECT

public:
    explicit TimelineView(QWidget* parent = nullptr);

    void setComposition(core::Composition* comp);
    [[nodiscard]] core::Composition* composition() const noexcept { return comp_; }

    [[nodiscard]] double currentTime() const noexcept { return currentTime_; }
    void setCurrentTime(double seconds);

    // Selection in insertion order; last is the primary, matching AE.
    [[nodiscard]] const std::vector<core::LayerId>& selectedLayers() const noexcept {
        return selected_;
    }

    // The primary selected layer, for commands that act on just one.
    [[nodiscard]] std::optional<core::LayerId> selectedLayer() const noexcept {
        return selected_.empty() ? std::optional<core::LayerId>{}
                                 : std::optional<core::LayerId>{selected_.back()};
    }
    [[nodiscard]] bool isSelected(core::LayerId layer) const noexcept;

    // Replaces the selection with this one layer.
    void selectLayer(core::LayerId layer);

    // How a click combines with what is already selected.
    enum class SelectMode {
        Replace,  // plain click: this layer and nothing else
        Toggle,   // cmd-click: add it, or take it out if it is already in
        Range,    // shift-click: everything between the primary and this one
    };
    void selectLayer(core::LayerId layer, SelectMode mode);

    // Empty selection is valid state (e.g. after Deselect All), not an error.
    void clearSelection();

    // Manual scroll (not QScrollArea) keeps the header/ruler pinned at y=0.
    void setScrollY(int y);
    [[nodiscard]] int contentHeight() const noexcept { return contentHeight_; }

    // Snaps drags to the playhead, other layers' edges, and rhythm markers.
    void setSnapping(bool on);
    [[nodiscard]] bool snapping() const noexcept { return snapping_; }

    // Borrowed peak pyramids keyed by media; an entry also means the layer has audio.
    using AudioPeaks = std::map<core::MediaId, media::PeakPyramid>;
    void setAudioPeaks(const AudioPeaks* peaks);

    // U: twirl open showing only animated properties (toggle). Distinct from the twirl
    // arrow, which shows everything. Matches AE.
    void revealAnimated(core::LayerId layer);

    // L: twirl open showing only Audio Level (toggle). No-op on a layer with no audio.
    // Matches AE's L, minus LL — Ruby's waveform is already always drawn, nothing to
    // reveal a second time.
    void revealAudioLevel(core::LayerId layer);

    // Clicking the twirl arrow. Always shows everything.
    void toggleExpanded(core::LayerId layer);


    // --- Horizontal zoom -----------------------------------------------------
    //
    // Maps a visible time WINDOW onto the track width, not the whole comp — otherwise a
    // long comp squeezes a short cut into one pixel. viewStart_/viewSpan_ are seconds,
    // not a zoom multiplier, since duration can change independently.
    [[nodiscard]] double viewStart() const noexcept { return viewStart_; }
    [[nodiscard]] double viewSpan() const noexcept { return viewSpan_; }
    void setViewStart(double seconds);

    // factor > 1 zooms in; anchorSeconds stays fixed under the cursor.
    void zoomBy(double factor, double anchorSeconds);
    void zoomToFit();

    // Sets window width directly, holding anchorSeconds fixed. zoomBy derives span from
    // a factor and calls this.
    void setViewSpan(double span, double anchorSeconds);

    // Narrowest allowed window, in seconds; depends on the comp's frame rate.
    [[nodiscard]] double minimumSpan() const noexcept;

    // Call when duration changes: keeps a fit-to-comp view fitted, leaves a zoomed view alone.
    void durationChanged();

signals:
    void currentTimeChanged(double seconds);

    // Primary layer (0 if none). Kept separate from selectionSetChanged so single-layer
    // consumers don't need the whole list.
    void selectionChanged(core::LayerId layer);

    // Fires on any selection-set change (add/remove/replace).
    void selectionSetChanged();

    void contentHeightChanged(int pixels);

    // A drag is one undo step, so the window brackets it rather than recording per move.
    void editBegan(const QString& label);
    void editEnded();
    void layersChanged();

    // The composition grew to contain a layer that ran past its end.
    void compositionResized(double seconds);

    // A speaker switch was toggled, so the mix has to be rebuilt.
    void audioChanged();

    // Right click on an effect header. Window owns removal (an undoable doc edit).
    void effectContextMenuRequested(int effectIndex, const QPoint& globalPos);

    // The visible window moved; horizontal scrollbar follows.
    void viewRangeChanged(double start, double span);

    // Right click on a layer row. Window owns the menu (Edit menu actions live there).
    void layerContextMenuRequested(const QPoint& globalPos);

    // Media dropped from the project panel. Window creates the layer; view only picks position.
    void mediaDropped(core::MediaId media, double seconds, int layerIndex);

    // An effect was dragged from the Effects panel onto a layer.
    void effectDropped(core::LayerId layer, const std::string& effectId);

protected:
    bool event(QEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    // Visible row kinds. Effects get their own header row, matching AE's twirl-open layout.
    enum class RowKind {
        Layer,
        EffectHeader,
        Property,
    };

    // Sentinel: an EffectHeader row with this index is the Transform group, not an effect.
    static constexpr int kTransformGroup = -1;

    struct Row {
        RowKind kind = RowKind::Layer;
        core::LayerId layer = 0;
        int effect = -1;         // -1 when the property belongs to the layer itself
        int propertyIndex = -1;  // unused for Layer and EffectHeader rows
        int top = 0;
        int height = 0;
    };

    void rebuildRows();
    [[nodiscard]] int trackLeft() const noexcept;
    [[nodiscard]] int trackWidth() const noexcept;
    [[nodiscard]] int navLeft() const noexcept;
    [[nodiscard]] int parentLeft() const noexcept;
    [[nodiscard]] int trkMatLeft() const noexcept;
    [[nodiscard]] int preserveLeft() const noexcept;
    [[nodiscard]] int modeLeft() const noexcept;
    [[nodiscard]] int switchesLeft() const noexcept;
    [[nodiscard]] QRect trackRect() const noexcept;
    [[nodiscard]] core::LayerId layerAtDrop(const QPoint& pos) const;
    [[nodiscard]] static bool carriesEffect(const QMimeData* mime);
    [[nodiscard]] double xForTime(double seconds) const noexcept;
    [[nodiscard]] double timeForX(int x) const noexcept;
    [[nodiscard]] double duration() const noexcept;
    [[nodiscard]] const media::PeakPyramid* peaksFor(const core::Layer& layer) const;

    // Keeps the window inside the composition and never lets it collapse.
    void clampView();

    // Ruler spacing that survives both a 3 second comp and a 3 hour one.
    [[nodiscard]] double tickInterval() const noexcept;

    // Ready-frame spans, pushed in by the window; keeps the timeline decoupled from the
    // compositor cache. A snapshot, not a per-pixel callback — querying while painting
    // would reorder eviction (a lookup counts as a hit).
public:
    struct CachedSpan {
        double start = 0.0;
        double end = 0.0;
        bool onDisk = false;  // green for RAM, blue for disk

        [[nodiscard]] bool operator==(const CachedSpan& o) const noexcept {
            return start == o.start && end == o.end && onDisk == o.onDisk;
        }
    };
    void setCachedSpans(std::vector<CachedSpan> spans);

private:
    void paintCacheBar(QPainter& p) const;
    void paintWorkArea(QPainter& p) const;
    [[nodiscard]] int rulerBottom() const noexcept;

    void paintHeader(QPainter& p) const;

    // AE's eight layer switches; only two are functional today, rest drawn inert for layout.
    void paintSwitches(QPainter& p, const Row& row, const core::Layer& layer) const;
    [[nodiscard]] int switchAt(int x) const noexcept;

    void paintLayerRow(QPainter& p, const Row& row, const core::Layer& layer) const;
    void paintPropertyRow(QPainter& p, const Row& row, const core::Layer& layer,
                          const core::Property& prop) const;
    void paintEffectHeader(QPainter& p, const Row& row, const core::Layer& layer) const;
    void paintGroupKeys(QPainter& p, const Row& row, const core::Layer& layer) const;

    // The group twirl's column, one step in from the layer's own.
    [[nodiscard]] static constexpr int groupTwirlLeft() noexcept { return 22; }
    static constexpr int kGroupTwirlW = 12;

    // Opens or shuts the Transform group or one effect's parameters. `effect` is
    // kTransformGroup for the former.
    void toggleGroup(core::LayerId layer, int effect);

    // Resolves a row's property, whether it lives on the layer or on one of its effects.
    [[nodiscard]] static const core::Property* propertyFor(const core::Layer& layer,
                                                           int effect, int index);
    void paintKeyNavigator(QPainter& p, const Row& row, bool hasKeyHere, bool canGoBack,
                           bool canGoForward) const;

    // Nearest keyframe before/after the playhead, across layer + effect properties.
    // `onKey` set when one sits exactly here.
    [[nodiscard]] bool nearestKey(const core::Layer& layer, bool forward, double& out,
                                  bool& onKey) const;

    void paintRhythm(QPainter& p) const;
    void paintPlayhead(QPainter& p) const;
    static void paintDiamond(QPainter& p, double cx, double cy, bool selected);

    // View state, not document state; kept separate from layer selection so selecting a
    // layer doesn't paint every one of its keys as selected.
    [[nodiscard]] std::optional<KeyRef> keyAt(const QPoint& pos) const;
    [[nodiscard]] bool isKeySelected(const KeyRef& ref) const;
    void toggleKeySelection(const KeyRef& ref, bool additive);

    // Distinct modes rather than a flag: move vs. trim behave differently under snapping.
    enum class DragMode {
        None,
        MoveLayer,
        TrimIn,
        TrimOut,
    };

    // Snaps to the nearest interesting time within a few pixels (ignores the dragged
    // layer). Threshold is in pixels, not seconds, since a fixed-time radius is wrong at
    // both zoom extremes.
    [[nodiscard]] double snapTime(double seconds, core::LayerId ignore) const;

    [[nodiscard]] DragMode hitTestBar(const core::Layer& layer, const QPoint& pos,
                                      const Row& row) const;

    core::Composition* comp_ = nullptr;
    std::vector<Row> rows_;
    std::vector<CachedSpan> cached_;

    // Which end of the work area a drag has hold of. None when nothing is being dragged.
    enum class WorkGrab { None, Start, End, Whole };
    WorkGrab workGrab_ = WorkGrab::None;
    double workGrabOffset_ = 0.0;

    bool snapping_ = true;

    // Layers with U pressed (twirled to animated-only). View state; not saved. Matches AE.
    std::set<core::LayerId> revealAnimated_;

    // Layers with L pressed (twirled to Audio Level only). View state; not saved.
    std::set<core::LayerId> audioLevelRevealed_;
    const AudioPeaks* audioPeaks_ = nullptr;

    // The layer an effect drag is currently over, or 0. Highlighted so the drop is not a
    // guess.
    core::LayerId dropEffectLayer_ = 0;

    // The visible time window. Span of 0 means "not set yet"; setComposition fits it.
    double viewStart_ = 0.0;
    double viewSpan_ = 0.0;

    // True while showing the whole comp; sticky so growth keeps a fitted view fitted.
    bool fit_ = true;
    DragMode dragMode_ = DragMode::None;
    core::LayerId dragLayer_ = 0;
    double dragGrabOffset_ = 0.0;  // seconds between the cursor and the layer's in point
    double dragOriginalIn_ = 0.0;
    double dragOriginalOut_ = 0.0;
    double currentTime_ = 3.14;
    int scrollY_ = 0;
    int contentHeight_ = 0;
    std::vector<core::LayerId> selected_;
    std::vector<KeyRef> selectedKeys_;
    bool scrubbing_ = false;

    // Pending drop location while a drag is in flight; dropRow_ == -1 means none pending.
    double dropTime_ = 0.0;
    int dropRow_ = -1;
};

// Timeline panel: the 26px sub-toolbar over the view.
class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    explicit TimelinePanel(QWidget* parent = nullptr);

    void setComposition(core::Composition* comp);

    // Rendered/cached spans (seconds), drawn under the ruler.
    void setCachedSpans(std::vector<TimelineView::CachedSpan> spans);

    // Refreshes sub-toolbar counts after an inspector edit (which can add a keyframe).
    void refresh();

    // Zoom, forwarded so the window can bind keys without reaching into the view.
    void zoomIn();
    void zoomOut();
    void zoomToFit();

    // Driven by playback; emits currentTimeChanged like a scrub so viewer/inspector stay in sync.
    void setCurrentTime(double seconds);
    void setSnapping(bool on);

    [[nodiscard]] std::optional<core::LayerId> selectedLayer() const;
    [[nodiscard]] const std::vector<core::LayerId>& selectedLayers() const;
    void selectLayer(core::LayerId layer);
    void clearSelection();
    void revealAnimated(core::LayerId layer);
    void revealAudioLevel(core::LayerId layer);
    void toggleExpanded(core::LayerId layer);
    void setAudioPeaks(const TimelineView::AudioPeaks* peaks);

signals:
    void currentTimeChanged(double seconds);
    void selectionChanged(core::LayerId layer);
    void selectionSetChanged();
    void editBegan(const QString& label);
    void editEnded();
    void layersChanged();
    void compositionResized(double seconds);
    void audioChanged();
    void effectDropped(core::LayerId layer, const std::string& effectId);
    void effectContextMenuRequested(int effectIndex, const QPoint& globalPos);
    void layerContextMenuRequested(const QPoint& globalPos);
    void mediaDropped(core::MediaId media, double seconds, int layerIndex);

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    class SubToolBar;

    void syncScrollRange();

    void syncTimeScrollRange();

    SubToolBar* bar_ = nullptr;
    TimelineView* view_ = nullptr;
    QScrollBar* scroll_ = nullptr;
    QScrollBar* timeScroll_ = nullptr;
    QSlider* zoom_ = nullptr;
};

}  // namespace ruby::ui
