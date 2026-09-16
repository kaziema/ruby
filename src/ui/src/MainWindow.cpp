#include "ruby/ui/MainWindow.h"

#include <QAction>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QMenu>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QStandardPaths>
#include <algorithm>
#include <limits>
#include <QCloseEvent>
#include <QFileInfo>
#include <QMessageBox>
#include <QMenuBar>
#include <QTimer>
#include <QPainter>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include "ruby/ui/DemoProject.h"
#include "ruby/ui/EditorToolBar.h"
#include "ruby/ui/EffectsPanel.h"
#include "ruby/ui/TextRaster.h"
#include "ruby/audio/AudioOutput.h"
#include "ruby/beat/Detector.h"
#include "ruby/media/AudioDecoder.h"
#include "ruby/media/PeakCache.h"
#include "ruby/media/Probe.h"
#include "ruby/ui/Format.h"
#include "ruby/ui/GpuViewport.h"
#include "ruby/ui/InspectorView.h"
#include "ruby/io/ProjectIO.h"
#include "ruby/ui/NewCompositionDialog.h"
#include "ruby/ui/NewSolidDialog.h"
#include "ruby/ui/NewTextDialog.h"
#include "ruby/ui/Playback.h"
#include "ruby/ui/PooledMediaPanel.h"
#include "ruby/ui/ProjectPanel.h"
#include "ruby/ui/StatusReadout.h"
#include "ruby/ui/PanelFrame.h"
#include "ruby/ui/Theme.h"
#include "ruby/ui/TimelineView.h"

namespace ruby::ui {

using namespace theme;

namespace {

// Stand-in for a rendered frame, until real footage replaces it.
class StripedCanvas : public QWidget {
public:
    explicit StripedCanvas(QString caption, QWidget* parent = nullptr)
        : QWidget(parent), caption_(std::move(caption)) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#131313"));

        p.setPen(QPen(QColor("#1c1c1c"), 2));
        const int span = width() + height();
        for (int i = -height(); i < span; i += 9) {
            p.drawLine(i, 0, i + height(), height());
        }

        p.setPen(QPen(QColor("#2a2a2a"), 1, Qt::DashLine));
        p.drawRect(rect().adjusted(24, 24, -24, -24));

        p.setFont(numericFont(type::kMeta));
        p.setPen(kTextDimmer);
        p.drawText(rect(), Qt::AlignCenter, caption_);
    }

private:
    QString caption_;
};

// Returns the page; timecodeOut receives the label so the timeline can drive it.
QWidget* makeViewerPage(QLabel** timecodeOut, GpuViewport** viewportOut) {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 0);
    layout->setSpacing(0);

    auto* canvas = new GpuViewport;
    layout->addWidget(canvas, 1);
    if (viewportOut != nullptr) {
        *viewportOut = canvas;
    }

    auto* bottom = new QWidget;
    bottom->setFixedHeight(metrics::kSubToolbarH);
    bottom->setAutoFillBackground(true);
    QPalette pal = bottom->palette();
    pal.setColor(QPalette::Window, kTabStrip);
    bottom->setPalette(pal);

    auto* bottomLayout = new QHBoxLayout(bottom);
    bottomLayout->setContentsMargins(9, 0, 9, 0);
    bottomLayout->setSpacing(14);

    const QFont mono = numericFont(type::kMeta);

    for (const QString& text : {QStringLiteral("42%"), QStringLiteral("00:00:00"),
                                QStringLiteral("Full"), QStringLiteral("Active Camera")}) {
        auto* label = new QLabel(text);
        label->setFont(mono);
        QPalette lp = label->palette();
        lp.setColor(QPalette::WindowText, kTextDim);
        label->setPalette(lp);
        bottomLayout->addWidget(label);
        if (text == QStringLiteral("00:00:00") && timecodeOut != nullptr) {
            *timecodeOut = label;
        }
    }
    bottomLayout->addStretch();

    layout->addSpacing(16);
    layout->addWidget(bottom);

    auto* wrap = new QWidget;
    auto* wrapLayout = new QVBoxLayout(wrap);
    wrapLayout->setContentsMargins(0, 0, 0, 0);
    wrapLayout->addWidget(page);
    wrap->setAutoFillBackground(true);
    QPalette wp = wrap->palette();
    wp.setColor(QPalette::Window, kGutter);
    wrap->setPalette(wp);
    return wrap;
}

}  // namespace

MainWindow::MainWindow(gpu::GpuDevice* device, QWidget* parent)
    : QMainWindow(parent), gpu_(device) {
    resize(1440, 900);

    buildMenus();

    auto* root = new QWidget;
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Pool lives in per-user app data (outlives any project); loaded before panels build
    // so the tab isn't empty on first show.
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    poolPath_ = dataDir + QStringLiteral("/media-pool.json");
    pool_.load(poolPath_.toStdString());

    peaksDir_ = dataDir + QStringLiteral("/peaks");
    QDir().mkpath(peaksDir_);

    // TEMPORARY: demo composition until the app can open a project file.
    project_ = demo::sampleProject();
    loadAudio();

    toolBar_ = new EditorToolBar;
    layout->addWidget(toolBar_);
    layout->addWidget(buildBody(), 1);

    setCentralWidget(root);

    setFocusPolicy(Qt::StrongFocus);
    setFocus();

    // Permanent widget: stays at the right end, unaffected by transient status messages.
    readout_ = new StatusReadout;
    statusBar()->addPermanentWidget(readout_);

    refreshCompositionTabs();
    refreshUndoActions();
    updateStatus();
    // Title derives from document state; set once at launch, not just on change.
    markClean();

    // Reports the actual frame rate achieved, not the target; a constant "30" is useless.
    auto* statusTick = new QTimer(this);
    statusTick->setInterval(250);
    connect(statusTick, &QTimer::timeout, this, [this] {
        if (playback_ != nullptr && playback_->playing()) {
            updateStatus();
        }
        // Cache bar refreshes on a timer, not per render: rebuilding per frame isn't free,
        // and 4x/sec is already faster than anyone can watch it fill.
        refreshCacheBar();
        updateReadouts();
    });
    statusTick->start();
}

namespace {

// macOS hides a QMenu with no actions; add real, disabled items instead of leaving
// menus empty so the menu bar doesn't silently collapse.
void addPending(QMenu* menu, const QStringList& items) {
    for (const QString& item : items) {
        if (item.isEmpty()) {
            menu->addSeparator();
            continue;
        }
        QAction* action = menu->addAction(item);
        action->setEnabled(false);
    }
}

}  // namespace

void MainWindow::importMedia() {
    // Broad filter, not exhaustive: probe() is the real gate, and FFmpeg reads far more
    // than any extension list we'd maintain.
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Import Media"), QString(),
        QStringLiteral("Media (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.wav *.mp3 *.aac "
                       "*.flac *.m4a *.ogg);;All files (*)"));
    if (paths.isEmpty()) {
        return;
    }

    recordEdit(QStringLiteral("Import Media"));

    int added = 0;
    int conformed = 0;
    bool pooledNew = false;
    QStringList rejected;
    for (const QString& path : paths) {
        const std::string local = path.toStdString();
        const auto info = media::probe(local);
        if (!info.has_value()) {
            rejected << QFileInfo(path).fileName();
            continue;
        }

        const core::MediaKind kind =
            info->hasVideo ? core::MediaKind::Video : core::MediaKind::Audio;
        project_.addMedia(local, QFileInfo(path).fileName().toStdString(), kind,
                          info->duration, info->width, info->height, info->fps,
                          info->hasAudio);

        // Also added to the app-level pool; firstSeen is stamped now since file timestamps
        // record creation, not import time.
        io::PooledItem pooled;
        pooled.path = local;
        pooled.name = QFileInfo(path).fileName().toStdString();
        pooled.kind = kind;
        pooled.duration = info->duration;
        pooled.bytes = QFileInfo(path).size();
        pooled.firstSeen = QDateTime::currentSecsSinceEpoch();
        if (pool_.add(pooled)) {
            pooledNew = true;
        }

        // Only conform files with audio; probe() already knows a silent clip needs no decode.
        if (info->hasAudio) {
            if (conformAudio(path) == media::ConformState::Ready) {
                ++conformed;
            }
        }
        ++added;
    }

    // Say what happened. A file that silently fails to import looks like a broken app.
    QString message = QStringLiteral("Imported %1 file%2")
                          .arg(added)
                          .arg(added == 1 ? QString() : QStringLiteral("s"));
    if (conformed > 0) {
        message += QStringLiteral("   ·   %1 with audio conformed").arg(conformed);
    }
    if (!rejected.isEmpty()) {
        message += QStringLiteral("   ·   could not read: %1").arg(rejected.join(", "));
    }
    statusBar()->showMessage(message, 6000);

    // Saved immediately, not at quit: a later crash shouldn't erase this record.
    if (pooledNew) {
        pool_.save(poolPath_.toStdString());
        if (pooledPanel_ != nullptr) {
            pooledPanel_->refresh();
        }
    }

    markDirty();
    emit mediaImported();
}

core::Composition* MainWindow::activeComposition() {
    if (activeComp_ != 0) {
        if (core::Composition* found = project_.find(activeComp_); found != nullptr) {
            return found;
        }
    }
    // Active comp was deleted or unset; fall back rather than pointing panels at nothing.
    if (project_.compositions().empty()) {
        return nullptr;
    }
    activeComp_ = project_.compositions().front().id;
    return &project_.compositions().front();
}

void MainWindow::beginEdit(const QString& label) {
    history_.beginGesture(project_, label.toStdString());
}

void MainWindow::endEdit() {
    history_.endGesture();
    markDirty();
    refreshUndoActions();
}

void MainWindow::recordEdit(const QString& label) {
    history_.record(project_, label.toStdString());
    markDirty();
    refreshUndoActions();
}

void MainWindow::refreshUndoActions() {
    if (undoAction_ != nullptr) {
        const std::string label = history_.undoLabel();
        undoAction_->setEnabled(history_.canUndo());
        undoAction_->setText(label.empty()
                                 ? QStringLiteral("Undo")
                                 : QStringLiteral("Undo %1")
                                       .arg(QString::fromStdString(label)));
    }
    if (redoAction_ != nullptr) {
        const std::string label = history_.redoLabel();
        redoAction_->setEnabled(history_.canRedo());
        redoAction_->setText(label.empty()
                                 ? QStringLiteral("Redo")
                                 : QStringLiteral("Redo %1")
                                       .arg(QString::fromStdString(label)));
    }
}

void MainWindow::afterDocumentReplaced() {
    // Undo swaps the whole document; re-point every panel's pointers before repaint.
    core::Composition* active = activeComposition();
    projectPanel_->setProject(&project_);
    if (viewport_ != nullptr) {
        viewport_->setProject(&project_);
        viewport_->setComposition(active);
    }
    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(active);
    }
    if (inspector_ != nullptr) {
        inspector_->setComposition(active);
    }
    refreshCompositionTabs();
    projectPanel_->refresh();
    updateStatus();
}

void MainWindow::undo() {
    if (!history_.undo(project_)) {
        return;
    }
    afterDocumentReplaced();
    markDirty();
    refreshUndoActions();
}

void MainWindow::redo() {
    if (!history_.redo(project_)) {
        return;
    }
    afterDocumentReplaced();
    markDirty();
    refreshUndoActions();
}

core::SizeOf MainWindow::layerSizes() {
    return [this](const core::Layer& layer) -> core::LayerSize {
        const core::Composition* comp = activeComposition();
        const double compW = comp != nullptr ? static_cast<double>(comp->width) : 0.0;
        const double compH = comp != nullptr ? static_cast<double>(comp->height) : 0.0;

        switch (layer.kind) {
            case core::LayerKind::Solid:
                // Zero means "match the composition", the same rule the compositor uses.
                return {layer.solidWidth > 0 ? static_cast<double>(layer.solidWidth) : compW,
                        layer.solidHeight > 0 ? static_cast<double>(layer.solidHeight)
                                              : compH};
            case core::LayerKind::Precomp:
                if (layer.source.has_value()) {
                    if (const core::Composition* src = project_.find(*layer.source);
                        src != nullptr) {
                        return {static_cast<double>(src->width),
                                static_cast<double>(src->height)};
                    }
                }
                return {compW, compH};
            case core::LayerKind::Text: {
                // Size of the rasterized IMAGE, not the glyph bounds: the raster pads by
                // stroke width, and the compositor sizes the quad from that image. Measured
                // via the same function the rasteriser uses so padding can't drift.
                const QSizeF size = textLayerSize(layer, 1.0);
                return {size.width(), size.height()};
            }
            case core::LayerKind::Audio:
                return {0.0, 0.0};  // no picture, so no box
            case core::LayerKind::Footage:
            default:
                if (layer.media.has_value()) {
                    if (const core::MediaItem* item = project_.findMedia(*layer.media);
                        item != nullptr && item->width > 0 && item->height > 0) {
                        return {static_cast<double>(item->width),
                                static_cast<double>(item->height)};
                    }
                }
                return {compW, compH};
        }
    };
}

// Which selected layers can actually be aligned: they need a picture and an unlocked one.
std::vector<core::LayerId> MainWindow::alignableSelection() {
    std::vector<core::LayerId> out;
    core::Composition* comp = activeComposition();
    if (comp == nullptr || timelinePanel_ == nullptr) {
        return out;
    }
    for (const core::LayerId id : timelinePanel_->selectedLayers()) {
        const core::Layer* layer = comp->find(id);
        // Locked layers aren't draggable, so not alignable; audio has no picture to line up.
        if (layer != nullptr && !layer->locked &&
            layer->kind != core::LayerKind::Audio) {
            out.push_back(id);
        }
    }
    return out;
}

void MainWindow::updateAlignAvailability() {
    if (alignPanel_ == nullptr) {
        return;
    }
    alignPanel_->setSelectionCount(static_cast<int>(alignableSelection().size()));
}

bool MainWindow::nudgeLayerBy(core::Layer& layer, double dx, double dy, double seconds,
                              const core::SizeOf& sizes) {
    core::Composition* comp = activeComposition();
    core::Property* position = layer.find("position");
    if (comp == nullptr || position == nullptr) {
        return false;
    }
    const double compW = static_cast<double>(comp->width);
    const double compH = static_cast<double>(comp->height);
    const core::TimeContext ctx = comp->timeContext();

    // Position is stored in the PARENT's space, so a composition-pixel delta must go
    // through the parent transform first (identity if unparented).
    if (layer.parent.has_value()) {
        if (const core::Layer* owner = comp->find(*layer.parent); owner != nullptr) {
            const core::Transform2D back =
                core::resolvedTransform(*comp, *owner, seconds, ctx, compW, compH, sizes)
                    .inverse();
            // Linear part only: a delta is a direction, not a point, so translation drops out.
            const double ux = back.applyX(dx, dy) - back.applyX(0.0, 0.0);
            const double uy = back.applyY(dx, dy) - back.applyY(0.0, 0.0);
            dx = ux;
            dy = uy;
        }
    }

    const double px = compW > 0.0 ? dx / compW * 100.0 : 0.0;
    const double py = compH > 0.0 ? dy / compH * 100.0 : 0.0;
    if (std::fabs(px) < 1e-9 && std::fabs(py) < 1e-9) {
        return false;
    }

    // Shifts every keyframe, not just the one at the playhead: aligning one frame would
    // silently move every other one out of place.
    const auto shift = [px, py](core::Property& prop) {
        if (prop.animated()) {
            for (core::Keyframe& k : prop.keys) {
                k.value = core::Value::vec2(k.value.c[0] + px, k.value.c[1] + py);
            }
        } else {
            prop.staticValue =
                core::Value::vec2(prop.staticValue.c[0] + px, prop.staticValue.c[1] + py);
        }
    };

    // Tried on a copy first: an expression can ignore Position entirely, so writing to it
    // doesn't guarantee movement. Check whether the box actually moved instead of
    // special-casing expressions.
    const core::Bounds before =
        core::layerBounds(*comp, layer, seconds, ctx, compW, compH, sizes);
    core::Layer trial = layer;
    if (core::Property* trialPos = trial.find("position"); trialPos != nullptr) {
        shift(*trialPos);
    }
    const core::Bounds after =
        core::layerBounds(*comp, trial, seconds, ctx, compW, compH, sizes);
    if (std::fabs(after.left - before.left) < 1e-6 &&
        std::fabs(after.top - before.top) < 1e-6) {
        return false;  // Position is not what decides where this layer is
    }

    shift(*position);
    return true;
}

void MainWindow::alignSelectedLayer(AlignPanel::Align edge, AlignPanel::Target target) {
    core::Composition* comp = activeComposition();
    const std::vector<core::LayerId> ids = alignableSelection();
    if (comp == nullptr || ids.empty()) {
        return;
    }

    const double compW = static_cast<double>(comp->width);
    const double compH = static_cast<double>(comp->height);
    const core::TimeContext ctx = comp->timeContext();
    const double seconds = playback_ != nullptr ? playback_->time() : 0.0;
    const core::SizeOf sizes = layerSizes();

    // What everything lines up against: the frame, or the box around the whole selection.
    core::Bounds against{0.0, 0.0, compW, compH};
    if (target == AlignPanel::Target::Selection) {
        bool first = true;
        for (const core::LayerId id : ids) {
            const core::Layer* layer = comp->find(id);
            if (layer == nullptr) {
                continue;
            }
            const core::Bounds b =
                core::layerBounds(*comp, *layer, seconds, ctx, compW, compH, sizes);
            against = first ? b
                            : core::Bounds{std::min(against.left, b.left),
                                           std::min(against.top, b.top),
                                           std::max(against.right, b.right),
                                           std::max(against.bottom, b.bottom)};
            first = false;
        }
        if (first) {
            return;
        }
    }

    // One undo step for the whole gesture, not one per layer moved.
    recordEdit(QStringLiteral("Align Layers"));
    bool movedAny = false;

    for (const core::LayerId id : ids) {
        core::Layer* layer = comp->find(id);
        if (layer == nullptr) {
            continue;
        }
        const core::Bounds box =
            core::layerBounds(*comp, *layer, seconds, ctx, compW, compH, sizes);
        if (box.width() <= 0.0 && box.height() <= 0.0) {
            continue;  // nothing with an edge to align
        }

        double dx = 0.0;
        double dy = 0.0;
        switch (edge) {
            case AlignPanel::Align::Left:    dx = against.left - box.left; break;
            case AlignPanel::Align::HCenter: dx = against.centerX() - box.centerX(); break;
            case AlignPanel::Align::Right:   dx = against.right - box.right; break;
            case AlignPanel::Align::Top:     dy = against.top - box.top; break;
            case AlignPanel::Align::VCenter: dy = against.centerY() - box.centerY(); break;
            case AlignPanel::Align::Bottom:  dy = against.bottom - box.bottom; break;
        }
        movedAny = nudgeLayerBy(*layer, dx, dy, seconds, sizes) || movedAny;
    }

    if (!movedAny) {
        return;
    }
    timelinePanel_->setComposition(comp);
    inspector_->setSelectedLayer(timelinePanel_->selectedLayer());
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    markDirty();
}

// Even gaps between the outermost two (which stay put); sorted by on-screen position,
// not layer-list order.
void MainWindow::distributeSelectedLayers(AlignPanel::Align axis) {
    core::Composition* comp = activeComposition();
    const std::vector<core::LayerId> ids = alignableSelection();
    if (comp == nullptr || ids.size() < 3) {
        return;  // with two there is nothing between them to space out
    }

    const double compW = static_cast<double>(comp->width);
    const double compH = static_cast<double>(comp->height);
    const core::TimeContext ctx = comp->timeContext();
    const double seconds = playback_ != nullptr ? playback_->time() : 0.0;
    const core::SizeOf sizes = layerSizes();
    const bool vertical = axis == AlignPanel::Align::Top ||
                          axis == AlignPanel::Align::VCenter ||
                          axis == AlignPanel::Align::Bottom;

    // Anchor point per axis-mode: near edge, center, or far edge.
    const auto anchorOf = [&](const core::Bounds& b) {
        switch (axis) {
            case AlignPanel::Align::Left:    return b.left;
            case AlignPanel::Align::HCenter: return b.centerX();
            case AlignPanel::Align::Right:   return b.right;
            case AlignPanel::Align::Top:     return b.top;
            case AlignPanel::Align::VCenter: return b.centerY();
            case AlignPanel::Align::Bottom:  return b.bottom;
        }
        return b.left;
    };

    struct Placed {
        core::Layer* layer = nullptr;
        double anchor = 0.0;
    };
    std::vector<Placed> placed;
    for (const core::LayerId id : ids) {
        core::Layer* layer = comp->find(id);
        if (layer == nullptr) {
            continue;
        }
        placed.push_back({layer, anchorOf(core::layerBounds(*comp, *layer, seconds, ctx,
                                                            compW, compH, sizes))});
    }
    if (placed.size() < 3) {
        return;
    }
    std::sort(placed.begin(), placed.end(),
              [](const Placed& a, const Placed& b) { return a.anchor < b.anchor; });

    const double first = placed.front().anchor;
    const double last = placed.back().anchor;
    const double step = (last - first) / static_cast<double>(placed.size() - 1);

    recordEdit(QStringLiteral("Distribute Layers"));
    bool movedAny = false;

    // Ends stay fixed; they define the span being distributed across.
    for (std::size_t i = 1; i + 1 < placed.size(); ++i) {
        const double want = first + step * static_cast<double>(i);
        const double delta = want - placed[i].anchor;
        movedAny = nudgeLayerBy(*placed[i].layer, vertical ? 0.0 : delta,
                                vertical ? delta : 0.0, seconds, sizes) ||
                   movedAny;
    }

    if (!movedAny) {
        return;
    }
    timelinePanel_->setComposition(comp);
    inspector_->setSelectedLayer(timelinePanel_->selectedLayer());
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    markDirty();
}

core::Layer* MainWindow::selectedLayer() {
    core::Composition* comp = activeComposition();
    if (comp == nullptr || timelinePanel_ == nullptr) {
        return nullptr;
    }
    const auto id = timelinePanel_->selectedLayer();
    return id.has_value() ? comp->find(*id) : nullptr;
}

// `[`/`]` move the layer so an edge lands on the playhead; Option+key trims instead.
void MainWindow::nudgeLayerEdge(bool inPoint, bool trim) {
    core::Composition* comp = activeComposition();
    core::Layer* layer = selectedLayer();
    if (comp == nullptr || layer == nullptr || playback_ == nullptr) {
        return;
    }
    const core::TimeContext ctx = comp->timeContext();
    const double t = playback_->time();
    const double in = to_seconds(layer->inPoint, ctx);
    const double out = to_seconds(layer->outPoint, ctx);
    const double minimum = 1.0 / std::max(1.0, comp->fps);

    recordEdit(trim ? QStringLiteral("Trim Layer") : QStringLiteral("Move Layer"));

    if (trim) {
        // Refused, not clamped: a zero-length bar can't be grabbed again.
        if (inPoint) {
            if (t >= out - minimum) {
                return;
            }
            layer->inPoint = core::TimeValue::seconds(std::max(0.0, t));
        } else {
            if (t <= in + minimum) {
                return;
            }
            layer->outPoint = core::TimeValue::seconds(t);
        }
    } else {
        const double length = out - in;
        const double newIn = std::max(0.0, inPoint ? t : t - length);
        layer->inPoint = core::TimeValue::seconds(newIn);
        layer->outPoint = core::TimeValue::seconds(newIn + length);
    }

    // Growing past the end extends the composition immediately: unlike a drag, a
    // keyboard nudge is a single discrete step with nothing to wait for.
    const bool grew = comp->growToFit();

    timelinePanel_->setComposition(comp);
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    markDirty();
    if (grew) {
        noteCompositionGrew();
    }
}

void MainWindow::splitLayerAtPlayhead() {
    core::Composition* comp = activeComposition();
    core::Layer* layer = selectedLayer();
    if (comp == nullptr || layer == nullptr || playback_ == nullptr) {
        return;
    }
    const core::TimeContext ctx = comp->timeContext();
    const double t = playback_->time();
    const double in = to_seconds(layer->inPoint, ctx);
    const double out = to_seconds(layer->outPoint, ctx);

    // Splitting outside the layer, or exactly on an edge, would make a zero-length half.
    if (t <= in || t >= out) {
        statusBar()->showMessage(
            QStringLiteral("Move the playhead inside the layer to split it"), 4000);
        return;
    }

    recordEdit(QStringLiteral("Split Layer"));

    // Tail is a full copy (media, effects, keyframes); keyframes are kept in both
    // halves, matching AE, so a split can be undone by trimming rather than re-animating.
    core::Layer tail = *layer;
    tail.id = comp->nextLayerId();
    tail.inPoint = core::TimeValue::seconds(t);
    tail.outPoint = core::TimeValue::seconds(out);

    layer->outPoint = core::TimeValue::seconds(t);

    const auto position = std::find_if(
        comp->layers.begin(), comp->layers.end(),
        [id = layer->id](const core::Layer& l) { return l.id == id; });
    const core::LayerId tailId = tail.id;
    comp->layers.insert(position, std::move(tail));  // the tail sits above the head

    timelinePanel_->setComposition(comp);
    timelinePanel_->selectLayer(tailId);
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    markDirty();
}

// U: reveal only the animated properties. Not the same as the twirl arrow, which shows
// everything. This used to just toggle `expanded`, which made the two identical and meant
// the twirl could never show an unanimated property.
void MainWindow::toggleSelectedLayerProperties() {
    core::Layer* layer = selectedLayer();
    if (layer == nullptr || timelinePanel_ == nullptr) {
        return;
    }
    timelinePanel_->revealAnimated(layer->id);
    markDirty();
}

// L: reveal the Audio Level property. A no-op on a layer with no audio.
void MainWindow::revealSelectedLayerAudioLevel() {
    core::Layer* layer = selectedLayer();
    if (layer == nullptr || timelinePanel_ == nullptr) {
        return;
    }
    timelinePanel_->revealAudioLevel(layer->id);
}

// J and K walk the keyframes of the selected layer, including the ones on its effects.
void MainWindow::jumpToKeyframe(bool forward) {
    core::Composition* comp = activeComposition();
    core::Layer* layer = selectedLayer();
    if (comp == nullptr || layer == nullptr || playback_ == nullptr) {
        return;
    }
    const core::TimeContext ctx = comp->timeContext();
    const double now = playback_->time();

    double best = forward ? std::numeric_limits<double>::max()
                          : std::numeric_limits<double>::lowest();
    bool found = false;

    const auto consider = [&](const core::Property& prop) {
        for (const core::Keyframe& k : prop.keys) {
            const double t = to_seconds(k.time, ctx);
            if (forward ? (t > now + 1e-6 && t < best) : (t < now - 1e-6 && t > best)) {
                best = t;
                found = true;
            }
        }
    };
    for (const core::Property& p : layer->properties) {
        consider(p);
    }
    for (const core::EffectInstance& fx : layer->effects) {
        for (const core::Property& p : fx.params) {
            consider(p);
        }
    }

    if (found) {
        playback_->seek(best);
    }
}

void MainWindow::newProject() {
    if (!confirmDiscard()) {
        return;
    }
    project_ = core::Project{};
    history_.clear();
    projectPath_.clear();
    activeComp_ = 0;
    // Silence the mixer BEFORE dropping buffers; the audio thread doesn't know the
    // project is going away.
    if (audioOut_ != nullptr) {
        audioOut_->stop();
        audioOut_->setSources({});
    }
    audio_.clear();
    peaks_.clear();
    if (timelinePanel_ != nullptr) {
        timelinePanel_->setAudioPeaks(nullptr);
    }
    rhythmNote_.clear();

    // Always create one composition; an empty project leaves nothing to work on.
    core::Composition& comp =
        project_.addComposition("Comp 1", 1080, 1920, 30.0, 15.0);
    setActiveComposition(comp.id);
    projectPanel_->refresh();
    markClean();
}

void MainWindow::openProject() {
    if (!confirmDiscard()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Project"), QString(),
        QStringLiteral("Ruby project (*.rbypr);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    openProject(path);
}

// Same as openProject(), with the file already chosen (command line, Finder double-click).
void MainWindow::openProject(const QString& path) {
    core::Project loaded;
    const io::LoadReport report = io::load(loaded, path.toStdString());
    if (!report.ok) {
        QMessageBox::warning(this, QStringLiteral("Could not open"),
                             QString::fromStdString(report.error));
        return;
    }

    // Migrates the loaded document onto current schemas: ranges get put back, effects run
    // their migration chain. Notes feed into the same report as missing-media warnings.
    std::vector<std::string> migrationNotes;
    for (core::Composition& comp : loaded.compositions()) {
        for (core::Layer& layer : comp.layers) {
            core::adoptTransformRanges(layer);
            core::ensureAudioLevel(layer);
            for (core::EffectInstance& fx : layer.effects) {
                engine::EffectRegistry::MigrationReport r =
                    engine::EffectRegistry::instance().migrate(fx);
                for (std::string& note : r.notes) {
                    migrationNotes.push_back(std::move(note));
                }
            }
        }
    }

    project_ = std::move(loaded);
    history_.clear();
    projectPath_ = path;
    activeComp_ = 0;
    if (audioOut_ != nullptr) {
        audioOut_->stop();
        audioOut_->setSources({});
    }
    audio_.clear();
    peaks_.clear();
    if (timelinePanel_ != nullptr) {
        timelinePanel_->setAudioPeaks(nullptr);
    }
    rhythmNote_.clear();

    core::Composition* active = activeComposition();
    if (active != nullptr) {
        setActiveComposition(active->id);
    }
    projectPanel_->setProject(&project_);
    if (viewport_ != nullptr) {
        viewport_->setProject(&project_);
    }
    refreshCompositionTabs();
    markClean();

    // Anything the loader repaired is reported, rather than silently opening incomplete.
    std::vector<std::string> allNotes = report.notes;
    for (std::string& note : migrationNotes) {
        allNotes.push_back(std::move(note));
    }
    if (!allNotes.empty()) {
        QStringList notes;
        for (const std::string& note : allNotes) {
            notes << QString::fromStdString(note);
        }
        QMessageBox::information(this, QStringLiteral("Opened with changes"),
                                 notes.join(QStringLiteral("\n")));
    }
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(QFileInfo(path).fileName()),
                             5000);
}

bool MainWindow::saveProject(bool forcePrompt) {
    QString path = projectPath_;
    if (path.isEmpty() || forcePrompt) {
        path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Project"),
            path.isEmpty() ? QStringLiteral("Untitled.rbypr") : path,
            QStringLiteral("Ruby project (*.rbypr)"));
        if (path.isEmpty()) {
            return false;
        }
        if (!path.endsWith(QStringLiteral(".rbypr"))) {
            path += QStringLiteral(".rbypr");
        }
    }

    std::string error;
    if (!io::save(project_, path.toStdString(), &error)) {
        QMessageBox::warning(this, QStringLiteral("Could not save"),
                             QString::fromStdString(error));
        return false;
    }

    projectPath_ = path;
    markClean();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()),
                             4000);
    return true;
}

bool MainWindow::confirmDiscard() {
    if (!dirty_) {
        return true;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Unsaved changes"),
        QStringLiteral("Save changes to this project first?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (answer == QMessageBox::Cancel) {
        return false;
    }
    if (answer == QMessageBox::Save) {
        return saveProject(false);
    }
    return true;
}

void MainWindow::markDirty() {
    if (!dirty_) {
        dirty_ = true;
        updateTitle();
    }

    // Hooked here instead of at each edit site, so no editor can forget to refresh audio.
    // Outside the `if`: dirty only flips once, but the mix must follow every edit after.
    rebuildMix();
}

void MainWindow::markClean() {
    dirty_ = false;
    updateTitle();
}

void MainWindow::updateTitle() {
    const QString name = projectPath_.isEmpty()
                             ? QStringLiteral("Untitled")
                             : QFileInfo(projectPath_).fileName();
    setWindowTitle(QStringLiteral("%1%2 — Ruby")
                       .arg(name, dirty_ ? QStringLiteral(" •") : QString()));
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (confirmDiscard()) {
        e->accept();
    } else {
        e->ignore();
    }
}

void MainWindow::newComposition() {
    NewCompositionDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const NewCompositionDialog::Settings s = dialog.settings();

    recordEdit(QStringLiteral("New Composition"));
    core::Composition& comp = project_.addComposition(
        s.name.toStdString(), s.width, s.height, s.fps, s.duration);
    setActiveComposition(comp.id);
    projectPanel_->refresh();
    markDirty();

    statusBar()->showMessage(QStringLiteral("Created %1  ·  %2x%3  ·  %4 fps  ·  %5s")
                                 .arg(s.name)
                                 .arg(s.width)
                                 .arg(s.height)
                                 .arg(s.fps, 0, 'g', 5)
                                 .arg(s.duration, 0, 'f', 1),
                             6000);
}

// The only way a composition gets shorter: duration grows automatically but never
// shrinks on its own, so shrinking always goes through here explicitly.
void MainWindow::compositionSettings() {
    core::Composition* comp = activeComposition();
    if (comp == nullptr) {
        return;
    }

    NewCompositionDialog::Settings current;
    current.name = QString::fromStdString(comp->name);
    current.width = comp->width;
    current.height = comp->height;
    current.fps = comp->fps;
    current.duration = comp->duration;

    NewCompositionDialog dialog(current, comp->contentEnd(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const NewCompositionDialog::Settings s = dialog.settings();
    if (s.name == current.name && s.width == current.width &&
        s.height == current.height && s.fps == current.fps &&
        s.duration == current.duration) {
        return;  // nothing to record
    }

    recordEdit(QStringLiteral("Composition Settings"));
    comp->name = s.name.toStdString();
    comp->width = s.width;
    comp->height = s.height;
    comp->fps = s.fps;

    // Layers are NOT trimmed to fit; they hang over the end rather than lose work to
    // tidy up a number.
    comp->duration = s.duration;

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
    }
    if (playback_ != nullptr) {
        playback_->configure(comp->duration, comp->fps);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    refreshCompositionTabs();
    projectPanel_->refresh();
    // A frame rate change retimes any layer whose points are stored in beats or frames.
    updateStatus();
    markDirty();

    statusBar()->showMessage(QStringLiteral("%1  ·  %2x%3  ·  %4 fps  ·  %5s")
                                 .arg(s.name)
                                 .arg(s.width)
                                 .arg(s.height)
                                 .arg(s.fps, 0, 'g', 5)
                                 .arg(s.duration, 0, 'f', 2),
                             6000);
}

// Duration isn't cosmetic: the transport loops on it, and every bar rescales at once,
// which looks like a rendering fault unless announced.
void MainWindow::noteCompositionGrew() {
    core::Composition* comp = activeComposition();
    if (comp == nullptr) {
        return;
    }
    if (playback_ != nullptr) {
        playback_->configure(comp->duration, comp->fps);
    }
    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    updateStatus();
    markDirty();
    statusBar()->showMessage(
        QStringLiteral("Composition extended to %1 to fit the layer")
            .arg(formatTimecode(comp->duration, comp->fps)),
        5000);
}

// --- Edit menu ---------------------------------------------------------------

void MainWindow::removeSelectedLayer(const QString& undoLabel) {
    core::Composition* comp = activeComposition();
    if (comp == nullptr || timelinePanel_ == nullptr) {
        return;
    }
    // Removes every selected layer, not just the primary one.
    const std::vector<core::LayerId> going = timelinePanel_->selectedLayers();
    if (going.empty()) {
        return;
    }

    // Computed before removal, while indices still apply: selects the layer that slides
    // into the topmost deleted slot.
    std::size_t index = comp->layers.size();
    for (const core::LayerId id : going) {
        const auto at = std::find_if(comp->layers.begin(), comp->layers.end(),
                                     [id](const core::Layer& l) { return l.id == id; });
        if (at != comp->layers.end()) {
            index = std::min(index,
                             static_cast<std::size_t>(
                                 std::distance(comp->layers.begin(), at)));
        }
    }

    // One undo step for the whole gesture, not one per layer.
    recordEdit(undoLabel);
    bool removedAny = false;
    for (const core::LayerId id : going) {
        removedAny = comp->removeLayer(id) || removedAny;
    }
    if (!removedAny) {
        return;
    }

    // Duration does NOT shrink back when the layer that stretched it is deleted.
    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
        if (comp->layers.empty()) {
            timelinePanel_->clearSelection();
        } else {
            const std::size_t next = std::min(index, comp->layers.size() - 1);
            timelinePanel_->selectLayer(comp->layers[next].id);
        }
    }
    if (inspector_ != nullptr) {
        inspector_->setComposition(comp);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    updateStatus();
    markDirty();
}

void MainWindow::deleteLayer() {
    if (selectedLayer() == nullptr) {
        statusBar()->showMessage(QStringLiteral("Select a layer to delete"), 4000);
        return;
    }
    removeSelectedLayer(QStringLiteral("Delete Layer"));
}

void MainWindow::copyLayer() {
    const core::Layer* layer = selectedLayer();
    if (layer == nullptr) {
        statusBar()->showMessage(QStringLiteral("Select a layer to copy"), 4000);
        return;
    }
    clipboard_ = *layer;
    statusBar()->showMessage(
        QStringLiteral("Copied %1").arg(QString::fromStdString(layer->name)), 4000);
}

void MainWindow::cutLayer() {
    const core::Layer* layer = selectedLayer();
    if (layer == nullptr) {
        statusBar()->showMessage(QStringLiteral("Select a layer to cut"), 4000);
        return;
    }
    clipboard_ = *layer;
    removeSelectedLayer(QStringLiteral("Cut Layer"));
}

void MainWindow::pasteLayer() {
    core::Composition* comp = activeComposition();
    if (comp == nullptr || !clipboard_.has_value()) {
        return;
    }
    recordEdit(QStringLiteral("Paste Layer"));

    core::Layer copy = *clipboard_;
    copy.id = comp->nextLayerId();

    // Layer ids only mean something within one composition: keep the parent link when
    // pasting into the same comp, drop it otherwise since the id may belong to someone else.
    if (copy.parent.has_value() && comp->find(*copy.parent) == nullptr) {
        copy.parent.reset();
    }

    comp->layers.insert(comp->layers.begin(), std::move(copy));
    const core::LayerId pasted = comp->layers.front().id;
    const bool grew = comp->growToFit();

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
        timelinePanel_->selectLayer(pasted);
    }
    if (inspector_ != nullptr) {
        inspector_->setComposition(comp);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    updateStatus();
    markDirty();
    statusBar()->showMessage(
        QStringLiteral("Pasted %1")
            .arg(QString::fromStdString(comp->layers.front().name)), 4000);
    if (grew) {
        noteCompositionGrew();
    }
}

void MainWindow::duplicateLayer() {
    core::Composition* comp = activeComposition();
    if (comp == nullptr || timelinePanel_ == nullptr ||
        timelinePanel_->selectedLayers().empty()) {
        statusBar()->showMessage(QStringLiteral("Select a layer to duplicate"), 4000);
        return;
    }
    const std::vector<core::LayerId> sources = timelinePanel_->selectedLayers();
    recordEdit(QStringLiteral("Duplicate Layer"));

    core::LayerId made = 0;
    for (const core::LayerId id : sources) {
        const core::Layer* layer = comp->find(id);
        if (layer == nullptr) {
            continue;
        }
        core::Layer copy = *layer;
        copy.id = comp->nextLayerId();
        // Parent is KEPT: duplicating stays in the same comp, so the link is still valid.
        // Note: duplicating a parent+child pair leaves the copied child following the
        // ORIGINAL parent, not the copy.
        copy.locked = false;  // a copy you cannot touch is not a useful copy

        // Inserted directly above the original, not atop the whole stack.
        const auto at = std::find_if(comp->layers.begin(), comp->layers.end(),
                                     [id](const core::Layer& l) { return l.id == id; });
        made = copy.id;
        comp->layers.insert(at, std::move(copy));
    }
    if (made == 0) {
        return;
    }

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
        timelinePanel_->selectLayer(made);
    }
    if (inspector_ != nullptr) {
        inspector_->setComposition(comp);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    updateStatus();
    markDirty();
}

// Reuses the menu bar's own QActions rather than fresh ones, so shortcuts can't drift.
void MainWindow::showLayerContextMenu(const QPoint& globalPos) {
    const bool hasLayer = selectedLayer() != nullptr;
    cutAction_->setEnabled(hasLayer);
    copyAction_->setEnabled(hasLayer);
    duplicateAction_->setEnabled(hasLayer);
    deleteAction_->setEnabled(hasLayer);
    splitAction_->setEnabled(hasLayer);
    pasteAction_->setEnabled(clipboard_.has_value());

    QMenu menu(this);
    menu.addAction(cutAction_);
    menu.addAction(copyAction_);
    menu.addAction(pasteAction_);
    menu.addAction(duplicateAction_);
    menu.addSeparator();
    menu.addAction(splitAction_);
    menu.addSeparator();
    menu.addAction(deleteAction_);
    menu.exec(globalPos);

    // Restored: these are the menu bar's own actions, and leaving one disabled would
    // silently break the Edit menu until the next right-click.
    cutAction_->setEnabled(true);
    copyAction_->setEnabled(true);
    pasteAction_->setEnabled(true);
    duplicateAction_->setEnabled(true);
    deleteAction_->setEnabled(true);
    splitAction_->setEnabled(true);
}

// --- Effects -----------------------------------------------------------------

void MainWindow::applyEffect(const std::string& effectId) {
    core::Composition* comp = activeComposition();
    if (comp == nullptr || timelinePanel_ == nullptr ||
        timelinePanel_->selectedLayers().empty()) {
        statusBar()->showMessage(QStringLiteral("Select a layer to apply an effect to"),
                                 4000);
        return;
    }
    const std::vector<core::LayerId> targets = timelinePanel_->selectedLayers();
    const engine::EffectRegistry& registry = engine::EffectRegistry::instance();
    const engine::EffectDef* def = registry.find(effectId);
    if (def == nullptr) {
        return;
    }

    recordEdit(QStringLiteral("Apply %1")
                   .arg(QString::fromStdString(def->schema.display_name)));

    // New effect goes on the END of the stack, acting on the result of what's above it.
    for (const core::LayerId id : targets) {
        core::Layer* target = comp->find(id);
        if (target == nullptr || target->locked) {
            continue;
        }
        target->effects.push_back(registry.instantiate(effectId));

        // Twirled open so the new effect is visible immediately.
        target->expanded = true;
    }

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
    }
    if (inspector_ != nullptr) {
        inspector_->setComposition(comp);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    updateStatus();
    markDirty();
    statusBar()->showMessage(
        QStringLiteral("Applied %1").arg(QString::fromStdString(def->schema.display_name)),
        4000);
}

void MainWindow::removeAllEffects() {
    core::Composition* comp = activeComposition();
    core::Layer* layer = selectedLayer();
    if (comp == nullptr || layer == nullptr || layer->effects.empty()) {
        return;
    }
    recordEdit(QStringLiteral("Remove All Effects"));
    layer->effects.clear();

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
    }
    if (inspector_ != nullptr) {
        inspector_->setComposition(comp);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    updateStatus();
    markDirty();
}

void MainWindow::removeEffect(int index) {
    core::Composition* comp = activeComposition();
    core::Layer* layer = selectedLayer();
    if (comp == nullptr || layer == nullptr || index < 0 ||
        index >= static_cast<int>(layer->effects.size())) {
        return;
    }
    recordEdit(QStringLiteral("Remove %1")
                   .arg(QString::fromStdString(
                       layer->effects[static_cast<std::size_t>(index)].displayName)));
    layer->effects.erase(layer->effects.begin() + index);

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
    }
    if (inspector_ != nullptr) {
        inspector_->setComposition(comp);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    updateStatus();
    markDirty();
}

// User picks the lane rather than guessing: beat grids are periodic/quantizable, vocal
// onsets are not, and which one fits a track is a fact about the music, not something
// to infer.
void MainWindow::runBeatAnalyzer() {
    core::Composition* comp = activeComposition();
    if (comp == nullptr) {
        return;
    }

    // First decoded audio layer, same rule loadAudio uses, so the two can't disagree.
    const media::AudioBuffer* track = nullptr;
    for (const core::Layer& layer : comp->layers) {
        if (layer.kind != core::LayerKind::Audio || !layer.media.has_value()) {
            continue;
        }
        if (const auto found = audio_.find(*layer.media); found != audio_.end()) {
            track = &found->second;
            break;
        }
    }
    if (track == nullptr) {
        statusBar()->showMessage(
            QStringLiteral("Beat Analyzer needs an audio layer in this composition"), 5000);
        return;
    }

    auto detector = beat::createDetector();
    if (detector == nullptr || !detector->available()) {
        // Says so plainly, rather than silently finding nothing.
        QMessageBox::information(
            this, QStringLiteral("Beat Analyzer"),
            QStringLiteral("This build has no rhythm analysis.\n\n%1")
                .arg(QString::fromUtf8(detector != nullptr ? detector->description()
                                                           : "No detector.")));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Beat Analyzer"));
    dialog.setModal(true);
    auto* layout = new QVBoxLayout(&dialog);

    auto* beats = new QRadioButton(QStringLiteral("Beats"), &dialog);
    auto* vocals = new QRadioButton(QStringLiteral("Vocals"), &dialog);
    beats->setChecked(true);
    layout->addWidget(beats);
    layout->addWidget(vocals);

    auto* help = new QLabel(
        QStringLiteral("Beats finds a periodic grid you can quantise to. Vocals finds "
                       "syllable onsets, which is what to use when the cuts follow the "
                       "words rather than the drums."),
        &dialog);
    help->setWordWrap(true);
    help->setMaximumWidth(360);
    help->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    layout->addWidget(help);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Analyse"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const beat::Lane lane = vocals->isChecked() ? beat::Lane::Vocal : beat::Lane::Beat;

    recordEdit(QStringLiteral("Analyse Audio"));
    const beat::Result result = detector->analyze(*track, lane);

    // setLane preserves hand-placed markers; re-analysing must not discard corrections.
    comp->rhythm.setLane(lane == beat::Lane::Vocal ? core::MarkerLane::Vocal
                                                   : core::MarkerLane::Beat,
                         result.markers);

    rhythmNote_ = QStringLiteral("%1 %2")
                      .arg(static_cast<int>(result.markers.size()))
                      .arg(lane == beat::Lane::Vocal ? QStringLiteral("vocal onsets")
                                                     : QStringLiteral("beats"));
    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
    }
    updateStatus();
    markDirty();
    statusBar()->showMessage(rhythmNote_, 5000);
}

// Composition matching a clip's size/fps/length/name, instead of making the user
// transcribe those numbers into the New Composition dialog by hand.
void MainWindow::compositionFromMedia(core::MediaId media) {
    const core::MediaItem* item = project_.findMedia(media);
    if (item == nullptr) {
        return;
    }
    recordEdit(QStringLiteral("New Composition from %1")
                   .arg(QString::fromStdString(item->name)));

    // Audio has no frame size; falls back to defaults rather than a 0x0 comp.
    const int width = item->width > 0 ? item->width : 1080;
    const int height = item->height > 0 ? item->height : 1920;
    const double fps = item->fps > 0.0 ? item->fps : 30.0;
    const double duration = item->duration > 0.0 ? item->duration : 15.0;

    core::Composition& comp =
        project_.addComposition(item->name, width, height, fps, duration);
    core::Layer& layer = project_.addLayer(comp, item->name,
                                           item->isVideo() ? core::LayerKind::Footage
                                                           : core::LayerKind::Audio);
    layer.media = media;
    layer.inPoint = core::TimeValue::seconds(0.0);
    layer.outPoint = core::TimeValue::seconds(duration);

    setActiveComposition(comp.id);
    projectPanel_->refresh();
    markDirty();
    statusBar()->showMessage(
        QStringLiteral("Created %1  ·  %2x%3  ·  %4 fps")
            .arg(QString::fromStdString(item->name)).arg(width).arg(height)
            .arg(fps, 0, 'g', 5),
        5000);
}

void MainWindow::deleteProjectItem(bool isComposition, std::uint64_t id) {
    if (isComposition) {
        // Refused rather than half-done: may be nested in another comp, and precomp-
        // reference cleanup doesn't exist yet.
        statusBar()->showMessage(
            QStringLiteral("Deleting compositions is not supported yet"), 4000);
        return;
    }
    const auto media = static_cast<core::MediaId>(id);
    const core::MediaItem* item = project_.findMedia(media);
    if (item == nullptr) {
        return;
    }
    const std::size_t used = project_.usageCount(media);

    // Confirmed before removal, not reported after, while the user can still say no.
    if (used > 0) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Remove media"),
            QStringLiteral("%1 is used by %2 layer%3.\n\nRemoving it leaves those layers "
                           "in place with no source. Continue?")
                .arg(QString::fromStdString(item->name))
                .arg(used)
                .arg(used == 1 ? QString() : QStringLiteral("s")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    recordEdit(QStringLiteral("Remove %1").arg(QString::fromStdString(item->name)));
    project_.removeMedia(media);

    projectPanel_->refresh();
    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(activeComposition());
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    updateStatus();
    markDirty();
}

// A frame with no tabs left has nothing to show, so it goes and the rest take the space.
//
// Recomputed from scratch rather than toggled per event: a tab move is a removal followed
// by an insertion, and the state in between is one where the source looks empty. Reacting
// to each half separately would flash a panel out and back in.
void MainWindow::updatePanelVisibility() {
    if (viewerTabs_ != nullptr) {
        viewerTabs_->setVisible(viewerTabs_->tabCount() > 0);
    }
    if (inspectorTabs_ != nullptr) {
        inspectorTabs_->setVisible(inspectorTabs_->tabCount() > 0);
    }

    // The left dock is two frames stacked behind the toolbar's Project and fx buttons, so
    // an empty one is not a gap in the layout, it is a button that leads nowhere. The dock
    // itself only disappears once both are empty.
    if (leftDock_ != nullptr) {
        const int project = projectTabs_ != nullptr ? projectTabs_->tabCount() : 0;
        const int effects = effectsTabs_ != nullptr ? effectsTabs_->tabCount() : 0;
        leftDock_->setVisible(project > 0 || effects > 0);

        // Do not leave the dock parked on an empty half when the other one has something.
        if (project == 0 && effects > 0) {
            leftDock_->setCurrentWidget(effectsTabs_);
        } else if (effects == 0 && project > 0) {
            leftDock_->setCurrentWidget(projectTabs_);
        }
    }
}

void MainWindow::deselectAll() {
    if (timelinePanel_ != nullptr) {
        timelinePanel_->clearSelection();
    }
}

// Conform: decode once, summarise into a peak pyramid, cache it. Same approach AE/
// Olive/Kdenlive use, since on-demand decode can't keep up with scrubbing.
media::ConformState MainWindow::conformAudio(const QString& path) {
    const std::string local = path.toStdString();
    const std::string cache = media::peakCachePath(peaksDir_.toStdString(), local);

    // Keyed on path+size+mtime, so a re-exported file under the same name never serves
    // stale peaks.
    media::PeakPyramid pyramid;
    if (pyramid.load(cache)) {
        return media::ConformState::Ready;
    }

    const auto buffer = media::AudioDecoder::decode(local);
    if (!buffer.has_value() || !buffer->valid()) {
        return media::ConformState::Failed;  // no audio track, or unreadable
    }

    pyramid = media::PeakPyramid::build(*buffer);
    if (pyramid.empty()) {
        return media::ConformState::Failed;
    }
    // A failed cache write isn't a conform failure; peaks are usable now, just re-done
    // next launch.
    pyramid.save(cache);
    return media::ConformState::Ready;
}

// --- Layer > New -------------------------------------------------------------

// Shared setup for new layers, so solid/null/etc. can't drift in placement or undo.
core::Layer* MainWindow::createLayer(const QString& undoLabel, const std::string& name,
                                     core::LayerKind kind) {
    core::Composition* comp = activeComposition();
    if (comp == nullptr) {
        statusBar()->showMessage(
            QStringLiteral("No composition open. Composition > New Composition first."),
            5000);
        return nullptr;
    }
    recordEdit(undoLabel);

    core::Layer& layer = project_.addLayer(*comp, name, kind);
    // Spans the whole composition; no source to derive a length from.
    layer.inPoint = core::TimeValue::seconds(0.0);
    layer.outPoint = core::TimeValue::seconds(comp->duration);

    // addLayer puts it on top; move it above the selection instead so it's easy to find.
    const core::LayerId made = layer.id;
    if (const auto selected = timelinePanel_ != nullptr ? timelinePanel_->selectedLayer()
                                                        : std::nullopt;
        selected.has_value() && *selected != made) {
        const auto from = std::find_if(comp->layers.begin(), comp->layers.end(),
                                       [made](const core::Layer& l) { return l.id == made; });
        if (from != comp->layers.end()) {
            core::Layer moved = std::move(*from);
            comp->layers.erase(from);
            const auto at = std::find_if(
                comp->layers.begin(), comp->layers.end(),
                [id = *selected](const core::Layer& l) { return l.id == id; });
            comp->layers.insert(at, std::move(moved));
        }
    }

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
        timelinePanel_->selectLayer(made);
    }
    if (inspector_ != nullptr) {
        inspector_->setComposition(comp);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    updateStatus();
    markDirty();
    return comp->find(made);
}

void MainWindow::newSolidLayer() {
    core::Composition* comp = activeComposition();
    if (comp == nullptr) {
        statusBar()->showMessage(
            QStringLiteral("No composition open. Composition > New Composition first."),
            5000);
        return;
    }
    NewSolidDialog dialog(comp->width, comp->height, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const NewSolidDialog::Settings s = dialog.settings();

    core::Layer* layer =
        createLayer(QStringLiteral("New Solid"), s.name.toStdString(),
                    core::LayerKind::Solid);
    if (layer == nullptr) {
        return;
    }
    // No gamma conversion: compositor works in linear light, matching what was picked
    // on screen.
    layer->solidColor = core::Value::rgba(
        static_cast<double>(s.color.redF()), static_cast<double>(s.color.greenF()),
        static_cast<double>(s.color.blueF()), 1.0);
    layer->solidWidth = s.width;
    layer->solidHeight = s.height;

    if (viewport_ != nullptr) {
        viewport_->update();
    }
    statusBar()->showMessage(QStringLiteral("Created solid %1").arg(s.name), 4000);
}

namespace {

void applyTextSettings(core::Layer& layer, const NewTextDialog::Settings& s) {
    layer.text = s.text.toStdString();
    layer.fontFamily = s.fontFamily.toStdString();
    layer.fontSize = s.fontSize;
    layer.tracking = s.tracking;
    layer.lineHeight = s.lineHeight;
    layer.strokeWidth = s.strokeWidth;
    layer.textAlign = s.align;
    layer.textColor = core::Value::rgba(
        static_cast<double>(s.color.redF()), static_cast<double>(s.color.greenF()),
        static_cast<double>(s.color.blueF()), static_cast<double>(s.color.alphaF()));
    layer.strokeColor = core::Value::rgba(static_cast<double>(s.strokeColor.redF()),
                                          static_cast<double>(s.strokeColor.greenF()),
                                          static_cast<double>(s.strokeColor.blueF()), 1.0);
}

NewTextDialog::Settings settingsFrom(const core::Layer& layer) {
    NewTextDialog::Settings s;
    s.text = QString::fromStdString(layer.text);
    s.fontFamily = QString::fromStdString(layer.fontFamily);
    s.fontSize = layer.fontSize;
    s.tracking = layer.tracking;
    s.lineHeight = layer.lineHeight;
    s.strokeWidth = layer.strokeWidth;
    s.align = layer.textAlign;
    s.color = QColor::fromRgbF(layer.textColor.c[0], layer.textColor.c[1],
                               layer.textColor.c[2], layer.textColor.c[3]);
    s.strokeColor = QColor::fromRgbF(layer.strokeColor.c[0], layer.strokeColor.c[1],
                                     layer.strokeColor.c[2], 1.0);
    return s;
}

}  // namespace

void MainWindow::newTextLayer() {
    NewTextDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const NewTextDialog::Settings s = dialog.settings();

    // Named after its text content, not a generic "Text N".
    QString name = s.text.split(QLatin1Char('\n')).first().trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("Text");
    }

    core::Layer* layer =
        createLayer(QStringLiteral("New Text Layer"), name.toStdString(),
                    core::LayerKind::Text);
    if (layer == nullptr) {
        return;
    }
    applyTextSettings(*layer, s);
    layer->label = core::LabelColor::Lavender;  // the design's colour for text and shape

    if (viewport_ != nullptr) {
        viewport_->update();
    }
    statusBar()->showMessage(QStringLiteral("Created text layer"), 4000);
}

// There is no text field in the inspector yet, so the dialog is also the editor.
void MainWindow::editTextLayer() {
    core::Layer* layer = selectedLayer();
    if (layer == nullptr || layer->kind != core::LayerKind::Text) {
        return;
    }
    NewTextDialog dialog(settingsFrom(*layer), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    recordEdit(QStringLiteral("Edit Text"));
    applyTextSettings(*layer, dialog.settings());

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(activeComposition());
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    markDirty();
}

void MainWindow::newNullLayer() {
    if (createLayer(QStringLiteral("New Null"), "Null", core::LayerKind::Null) != nullptr) {
        statusBar()->showMessage(QStringLiteral("Created null"), 4000);
    }
}

void MainWindow::setActiveComposition(core::CompId id) {
    core::Composition* comp = project_.find(id);
    if (comp == nullptr) {
        return;
    }
    activeComp_ = id;

    // Every panel showing a composition must be re-told, or it keeps rendering the old
    // one. Decode cache is kept (keyed by media, not comp); loadAudio republishes the mix.
    rhythmNote_.clear();
    loadAudio();

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(comp);
    }
    if (viewport_ != nullptr) {
        viewport_->setComposition(comp);
    }
    if (inspector_ != nullptr) {
        inspector_->setComposition(comp);
    }
    if (playback_ != nullptr) {
        playback_->configure(comp->duration, comp->fps);
        playback_->seek(0.0);
    }
    refreshCompositionTabs();
    updateStatus();
}

void MainWindow::refreshCompositionTabs() {
    if (timelineTabs_ == nullptr) {
        return;
    }
    QStringList names;
    for (const core::Composition& comp : project_.compositions()) {
        names << QString::fromStdString(comp.name);
    }
    if (names.isEmpty()) {
        names << QStringLiteral("No composition");
    }
    timelineTabs_->setTabs(names);

    // Viewer tab is RENAMED, not replaced. Found wherever the page now lives rather than
    // assumed to still be in the viewer, since tabs can move between panels.
    if (viewerPage_ != nullptr) {
        int index = -1;
        if (PanelFrame* home = PanelFrame::frameHolding(viewerPage_, &index);
            home != nullptr) {
            const core::Composition* active = activeComposition();
            home->setTabLabel(index, QStringLiteral("Composition: %1")
                                         .arg(active != nullptr
                                                  ? QString::fromStdString(active->name)
                                                  : QStringLiteral("none")));
        }
    }
}

void MainWindow::dropMediaIntoComposition(core::MediaId id, double seconds,
                                          int layerIndex) {
    core::Composition* comp = activeComposition();
    const core::MediaItem* item = project_.findMedia(id);
    if (comp == nullptr || item == nullptr) {
        return;
    }
    recordEdit(QStringLiteral("Add %1").arg(QString::fromStdString(item->name)));

    core::Layer& layer = project_.addLayer(*comp, item->name,
                                           item->isVideo() ? core::LayerKind::Footage
                                                           : core::LayerKind::Audio);
    layer.media = id;

    // Drop position is the clip's start, not center. Layer keeps its SOURCE length even
    // past the composition end (as AE does); clamping would silently discard footage.
    const double start = std::max(0.0, seconds);
    const double length = item->duration > 0.0 ? item->duration : comp->duration;
    layer.inPoint = core::TimeValue::seconds(start);
    layer.outPoint = core::TimeValue::seconds(start + length);
    const bool grew = comp->growToFit();

    // addLayer puts it on top; move it to where it was dropped in the stack.
    const auto placed = std::find_if(
        comp->layers.begin(), comp->layers.end(),
        [id = layer.id](const core::Layer& l) { return l.id == id; });
    if (placed != comp->layers.end()) {
        core::Layer moved = std::move(*placed);
        const core::LayerId movedId = moved.id;
        comp->layers.erase(placed);
        const auto at = std::min(static_cast<std::size_t>(std::max(0, layerIndex)),
                                 comp->layers.size());
        comp->layers.insert(comp->layers.begin() + static_cast<long>(at),
                            std::move(moved));

        if (timelinePanel_ != nullptr) {
            timelinePanel_->setComposition(comp);
            timelinePanel_->selectLayer(movedId);
        }
    }

    // Any clip that carries audio, video included. loadAudio owns publishing the mix.
    if (item->hasAudio) {
        loadAudio();
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    projectPanel_->refresh();
    updateStatus();
    markDirty();

    // Shown last, so it wins the status bar over the less useful "Added clip.mov".
    if (grew) {
        noteCompositionGrew();
    }
}

void MainWindow::addMediaToComposition(core::MediaId id) {
    core::Composition* active = activeComposition();
    if (active == nullptr) {
        statusBar()->showMessage(
            QStringLiteral("No composition open. Composition > New Composition first."),
            5000);
        return;
    }
    const core::MediaItem* item = project_.findMedia(id);
    if (item == nullptr) {
        return;
    }
    core::Composition& comp = *active;
    recordEdit(QStringLiteral("Add %1").arg(QString::fromStdString(item->name)));

    const core::LayerKind kind =
        item->isVideo() ? core::LayerKind::Footage : core::LayerKind::Audio;
    core::Layer& layer = project_.addLayer(comp, item->name, kind);
    layer.media = id;

    // Enters at start, full source length even past the comp end; trimming is manual.
    layer.inPoint = core::TimeValue::seconds(0.0);
    layer.outPoint = core::TimeValue::seconds(
        item->duration > 0.0 ? item->duration : comp.duration);
    const bool grew = comp.growToFit();

    if (item->hasAudio) {
        loadAudio();
    }

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setComposition(&comp);
    }
    if (viewport_ != nullptr) {
        viewport_->update();
    }
    projectPanel_->refresh();
    updateStatus();
    markDirty();

    statusBar()->showMessage(
        QStringLiteral("Added %1").arg(QString::fromStdString(item->name)), 4000);
    if (grew) {
        noteCompositionGrew();
    }
}

// Republishes the mix from the layers exactly as they stand.
//
// This is the fix for audio staying where a clip used to be. Dragging a bar mutates the
// layer's in and out points and repaints, but the mixer holds its own copy of those times
// and nothing was telling it they had moved. The clip slid across the timeline and its
// sound stayed behind, which looks like a sync bug and is really a stale-copy bug.
//
// Cheap on purpose: it runs on every mouse move of a drag, so it only reads buffers that
// are already decoded and never analyses anything.
void MainWindow::rebuildMix() {
    if (audioOut_ == nullptr) {
        return;
    }
    core::Composition* comp = activeComposition();
    if (comp == nullptr) {
        audioOut_->setSources({});
        return;
    }
    const core::TimeContext ctx = comp->timeContext();

    // Solo applies to sound independently of picture: check whether any AUDIBLE layer
    // is soloed, not any layer.
    bool anySolo = false;
    for (const core::Layer& layer : comp->layers) {
        if (layer.solo && layer.media.has_value() &&
            audio_.find(*layer.media) != audio_.end()) {
            anySolo = true;
            break;
        }
    }

    const double now = playback_ != nullptr ? playback_->time() : 0.0;

    std::vector<audio::AudioSource> sources;
    for (const core::Layer& layer : comp->layers) {
        if (!layer.media.has_value() || !layer.audioEnabled) {
            continue;
        }
        if (anySolo && !layer.solo) {
            continue;
        }
        const auto found = audio_.find(*layer.media);
        if (found == audio_.end()) {
            continue;  // not decoded yet; loadAudio will pick it up
        }
        audio::AudioSource source;
        source.buffer = &found->second;
        source.startSeconds = to_seconds(layer.inPoint, ctx);
        source.endSeconds = to_seconds(layer.outPoint, ctx);
        source.sourceOffset = 0.0;
        // Stored as linear gain already, so this needs no conversion — the inspector
        // does the dB round trip, not the mixer.
        if (const core::Property* level = layer.find(core::kAudioLevelKey);
            level != nullptr) {
            source.gain = static_cast<float>(level->evaluate(now, ctx).x());
        }
        sources.push_back(source);
    }
    audioOut_->setSources(sources);
}

// Every layer whose media carries audio is a source, video included: audibility is a
// fact about the media, not the layer's kind.
void MainWindow::loadAudio() {
    core::Composition* active = activeComposition();
    if (active == nullptr) {
        return;
    }
    core::Composition& comp = *active;

    const media::AudioBuffer* rhythmSource = nullptr;
    std::optional<core::MediaId> rhythmMedia;

    for (core::Layer& layer : comp.layers) {
        if (!layer.media.has_value()) {
            continue;
        }
        const core::MediaItem* item = project_.findMedia(*layer.media);
        if (item == nullptr || !item->hasAudio) {
            continue;
        }
        const std::string path = project_.pathFor(layer);
        if (path.empty()) {
            continue;
        }

        // Decoded once per media item; layers cutting the same clip share the buffer.
        auto found = audio_.find(*layer.media);
        if (found == audio_.end()) {
            auto decoded = media::AudioDecoder::decode(path);
            if (!decoded.has_value() || !decoded->valid()) {
                continue;
            }
            found = audio_.emplace(*layer.media, std::move(*decoded)).first;
        }
        const media::AudioBuffer& buffer = found->second;

        // Peaks for drawing: try the conform cache first, build only on a miss.
        if (peaks_.find(*layer.media) == peaks_.end()) {
            media::PeakPyramid pyramid;
            if (!pyramid.load(media::peakCachePath(peaksDir_.toStdString(), path))) {
                pyramid = media::PeakPyramid::build(buffer);
                pyramid.save(media::peakCachePath(peaksDir_.toStdString(), path));
            }
            if (!pyramid.empty()) {
                peaks_.emplace(*layer.media, std::move(pyramid));
            }
        }

        if (rhythmSource == nullptr && layer.kind == core::LayerKind::Audio) {
            rhythmSource = &buffer;
            rhythmMedia = *layer.media;
        }
    }

    if (timelinePanel_ != nullptr) {
        timelinePanel_->setAudioPeaks(&peaks_);
    }
    rebuildMix();

    // Runs on a dedicated audio layer, not the mix: cutting to music shouldn't cut to
    // music-plus-dialogue. Only re-runs when the analyzed track actually changed.
    if (rhythmSource != nullptr && analyzedRhythmFor_ != rhythmMedia) {
        analyzedRhythmFor_ = rhythmMedia;
        auto detector = beat::createDetector();
        if (detector != nullptr && detector->available()) {
            const beat::Result vocal = detector->analyze(*rhythmSource, beat::Lane::Vocal);
            if (!vocal.empty()) {
                comp.rhythm.setLane(core::MarkerLane::Vocal, vocal.markers);
            }
            rhythmNote_ = QStringLiteral("%1 vocal onsets")
                              .arg(static_cast<int>(vocal.markers.size()));
        } else {
            rhythmNote_ = QStringLiteral("no rhythm analysis in this build");
        }
    }
}

void MainWindow::updateStatus() {
    QString text = (gpu_ != nullptr)
                       ? QStringLiteral("GPU: %1").arg(
                             QString::fromStdString(gpu_->description()))
                       : QStringLiteral("No GPU adapter. Viewport disabled.");

    if (playback_ != nullptr && playback_->playing()) {
        text += QStringLiteral("   ·   playing %1 fps")
                    .arg(playback_->measuredFps(), 0, 'f', 1);
    } else {
        text += QStringLiteral("   ·   space to play");
    }
    if (!rhythmNote_.isEmpty()) {
        text += QStringLiteral("   ·   %1").arg(rhythmNote_);
    }
    statusBar()->showMessage(text);

    updateReadouts();
}

void MainWindow::updateReadouts() {
    if (readout_ == nullptr) {
        return;
    }
    std::vector<StatusReadout::Item> items;

    if (viewport_ != nullptr && activeComposition() != nullptr) {
        const engine::FrameCache::Stats s = viewport_->cacheStats();
        const double mb = static_cast<double>(s.bytes) / (1024.0 * 1024.0);
        const double budget = static_cast<double>(s.budget) / (1024.0 * 1024.0);
        items.push_back({QStringLiteral("Cache %1 / %2 MB")
                             .arg(mb, 0, 'f', 0)
                             .arg(budget, 0, 'f', 0),
                         s.bytes > 0 ? theme::kCacheReady : theme::kTextFaint, false});
    }

    readout_->setItems(std::move(items));
}

// Coalesces individual cached frames into runs here (not in the painter), since the
// bar wants "this stretch is ready", not one rectangle per frame.
void MainWindow::refreshCacheBar() {
    if (timelinePanel_ == nullptr || viewport_ == nullptr) {
        return;
    }
    core::Composition* comp = activeComposition();
    if (comp == nullptr) {
        return;
    }

    double from = 0.0;
    double to = 0.0;
    comp->workRange(from, to);

    const double fps = comp->fps > 0.0 ? comp->fps : 30.0;
    const double frame = 1.0 / fps;
    std::vector<TimelineView::CachedSpan> spans;

    for (const auto& [t, onDisk] : viewport_->cachedFrames(from, to)) {
        if (!spans.empty() && spans.back().onDisk == onDisk &&
            t - spans.back().end < frame * 1.5) {
            spans.back().end = t + frame;
            continue;
        }
        spans.push_back({t, t + frame, onDisk});
    }
    timelinePanel_->setCachedSpans(std::move(spans));
}

void MainWindow::buildMenus() {
    // Menus, in order:
    // File, Edit, Composition, Layer, Effect, Animation, View, Window, Help.
    auto* file = menuBar()->addMenu(QStringLiteral("File"));
    file->addAction(QStringLiteral("New Project"),
                    QKeySequence(QStringLiteral("Ctrl+Shift+N")), this,
                    &MainWindow::newProject);
    file->addAction(QStringLiteral("Open Project..."), QKeySequence::Open, this,
                    [this] { openProject(); });
    file->addAction(QStringLiteral("Save Project"), QKeySequence::Save, this,
                    [this] { saveProject(false); });
    file->addAction(QStringLiteral("Save Project As..."), QKeySequence::SaveAs, this,
                    [this] { saveProject(true); });
    file->addSeparator();
    file->addAction(QStringLiteral("Import Media..."), QKeySequence(QStringLiteral("Ctrl+I")),
                    this, &MainWindow::importMedia);
    addPending(file, {QStringLiteral("Import Preset Pack..."), QString(),
                      QStringLiteral("Export...")});
    file->addSeparator();
    file->addAction(QStringLiteral("Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto* edit = menuBar()->addMenu(QStringLiteral("Edit"));
    undoAction_ = edit->addAction(QStringLiteral("Undo"), QKeySequence::Undo, this,
                                  &MainWindow::undo);
    redoAction_ = edit->addAction(QStringLiteral("Redo"), QKeySequence::Redo, this,
                                  &MainWindow::redo);
    edit->addSeparator();
    cutAction_ = edit->addAction(QStringLiteral("Cut"), QKeySequence::Cut, this,
                                &MainWindow::cutLayer);
    copyAction_ = edit->addAction(QStringLiteral("Copy"), QKeySequence::Copy, this,
                                  &MainWindow::copyLayer);
    pasteAction_ = edit->addAction(QStringLiteral("Paste"), QKeySequence::Paste, this,
                                   &MainWindow::pasteLayer);
    duplicateAction_ = edit->addAction(QStringLiteral("Duplicate"),
                                       QKeySequence(QStringLiteral("Ctrl+D")), this,
                                       &MainWindow::duplicateLayer);
    deleteAction_ = edit->addAction(QStringLiteral("Delete"), QKeySequence::Delete, this,
                                    &MainWindow::deleteLayer);
    edit->addSeparator();
    // Select All stays pending until multi-layer selection exists.
    addPending(edit, {QStringLiteral("Select All")});
    edit->addAction(QStringLiteral("Deselect All"),
                    QKeySequence(QStringLiteral("Ctrl+Shift+A")), this,
                    &MainWindow::deselectAll);

    auto* comp = menuBar()->addMenu(QStringLiteral("Composition"));
    comp->addAction(QStringLiteral("New Composition..."),
                    QKeySequence(QStringLiteral("Ctrl+N")), this,
                    &MainWindow::newComposition);
    comp->addAction(QStringLiteral("Composition Settings..."),
                    QKeySequence(QStringLiteral("Ctrl+K")), this,
                    &MainWindow::compositionSettings);
    addPending(comp, {QString(),
                      QStringLiteral("Analyze Audio for Beats"),
                      QStringLiteral("Edit Beat Map..."),
                      QStringLiteral("Cut to Beats"), QString(),
                      QStringLiteral("Add to Render Queue")});

    auto* layer = menuBar()->addMenu(QStringLiteral("Layer"));
    splitAction_ = layer->addAction(QStringLiteral("Split at Playhead"),
                                    QKeySequence(QStringLiteral("Ctrl+Shift+D")), this,
                                    &MainWindow::splitLayerAtPlayhead);
    layer->addSeparator();
    layer->addAction(QStringLiteral("Move In Point to Playhead"),
                     QKeySequence(Qt::Key_BracketLeft), this,
                     [this] { nudgeLayerEdge(true, false); });
    layer->addAction(QStringLiteral("Move Out Point to Playhead"),
                     QKeySequence(Qt::Key_BracketRight), this,
                     [this] { nudgeLayerEdge(false, false); });
    layer->addAction(QStringLiteral("Trim In Point to Playhead"),
                     QKeySequence(Qt::ALT | Qt::Key_BracketLeft), this,
                     [this] { nudgeLayerEdge(true, true); });
    layer->addAction(QStringLiteral("Trim Out Point to Playhead"),
                     QKeySequence(Qt::ALT | Qt::Key_BracketRight), this,
                     [this] { nudgeLayerEdge(false, true); });
    layer->addSeparator();
    // AE's shortcuts, because muscle memory is the whole reason to match them.
    layer->addAction(QStringLiteral("New Solid..."),
                     QKeySequence(QStringLiteral("Ctrl+Y")), this,
                     &MainWindow::newSolidLayer);
    layer->addAction(QStringLiteral("New Null"),
                     QKeySequence(QStringLiteral("Ctrl+Alt+Shift+Y")), this,
                     &MainWindow::newNullLayer);
    layer->addAction(QStringLiteral("New Text Layer..."),
                     QKeySequence(QStringLiteral("Ctrl+Alt+Shift+T")), this,
                     &MainWindow::newTextLayer);
    layer->addAction(QStringLiteral("Text Settings..."),
                     QKeySequence(QStringLiteral("Ctrl+Shift+T")), this,
                     &MainWindow::editTextLayer);
    addPending(layer, {QStringLiteral("New Shape Layer"),
                       QStringLiteral("New Adjustment Layer"), QString(),
                       QStringLiteral("Pre-compose..."), QString(),
                       QStringLiteral("Add Mask"), QStringLiteral("Auto-Roto Subject..."),
                       QString(), QStringLiteral("Time Remap"),
                       QStringLiteral("Retime with Optical Flow...")});

    // Built from the registry, not hand-written, so the menu can't claim an effect that
    // doesn't exist.
    auto* effect = menuBar()->addMenu(QStringLiteral("Effect"));
    {
        std::map<QString, QMenu*> categories;
        for (const engine::EffectDef& def : engine::EffectRegistry::instance().all()) {
            const QString category =
                QString::fromStdString(engine::effectCategory(def.schema.id));
            QMenu*& submenu = categories[category];
            if (submenu == nullptr) {
                submenu = effect->addMenu(category);
            }
            submenu->addAction(QString::fromStdString(def.schema.display_name), this,
                               [this, id = def.schema.id] { applyEffect(id); });
        }
        effect->addSeparator();
        effect->addAction(QStringLiteral("Remove All Effects"),
                          QKeySequence(QStringLiteral("Ctrl+Shift+E")), this,
                          &MainWindow::removeAllEffects);
    }

    auto* anim = menuBar()->addMenu(QStringLiteral("Animation"));
    anim->addAction(QStringLiteral("Reveal Animated Properties"),
                    QKeySequence(Qt::Key_U), this,
                    &MainWindow::toggleSelectedLayerProperties);
    anim->addAction(QStringLiteral("Reveal Audio Level"), QKeySequence(Qt::Key_L), this,
                    &MainWindow::revealSelectedLayerAudioLevel);
    anim->addAction(QStringLiteral("Previous Keyframe"), QKeySequence(Qt::Key_J), this,
                    [this] { jumpToKeyframe(false); });
    anim->addAction(QStringLiteral("Next Keyframe"), QKeySequence(Qt::Key_K), this,
                    [this] { jumpToKeyframe(true); });
    anim->addSeparator();
    addPending(anim, {QStringLiteral("Add Keyframe"), QStringLiteral("Toggle Hold Keyframe"),
                      QString(), QStringLiteral("Keyframe Assistant..."),
                      QStringLiteral("Snap Keyframes to Beat"),
                      QStringLiteral("Stagger Selection..."), QString(),
                      QStringLiteral("Save Animation Preset..."),
                      QStringLiteral("Apply Animation Preset...")});

    auto* view = menuBar()->addMenu(QStringLiteral("View"));
    // Named "...Timeline" explicitly: AE's View > Zoom In means the viewer, so reusing
    // that label here would misname which panel it affects.
    view->addAction(QStringLiteral("Zoom In Timeline"),
                    QKeySequence(QStringLiteral("=")), this,
                    [this] { timelinePanel_->zoomIn(); });
    view->addAction(QStringLiteral("Zoom Out Timeline"),
                    QKeySequence(QStringLiteral("-")), this,
                    [this] { timelinePanel_->zoomOut(); });
    view->addAction(QStringLiteral("Fit Timeline to Window"),
                    QKeySequence(QStringLiteral(";")), this,
                    [this] { timelinePanel_->zoomToFit(); });
    view->addSeparator();
    addPending(view, {QStringLiteral("Zoom In"), QStringLiteral("Zoom Out"),
                      QStringLiteral("Fit to Window"), QString(),
                      QStringLiteral("Show Guides"), QStringLiteral("Show Title/Action Safe"),
                      QStringLiteral("Show Beat Grid"), QString(),
                      QStringLiteral("Resolution")});

    auto* window = menuBar()->addMenu(QStringLiteral("Window"));
    addPending(window, {QStringLiteral("Project"), QStringLiteral("Composition"),
                        QStringLiteral("Inspector"), QStringLiteral("Timeline"),
                        QStringLiteral("Effects && Presets"), QStringLiteral("Keyframes"),
                        QString(), QStringLiteral("Reset Workspace")});

    auto* help = menuBar()->addMenu(QStringLiteral("Help"));
    addPending(help, {QStringLiteral("Composition Help"),
                      QStringLiteral("Keyboard Shortcuts"), QString(),
                      QStringLiteral("Release Notes")});
}

QWidget* MainWindow::makePlaceholder(const QString& note) {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);

    auto* label = new QLabel(note);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    QFont f = label->font();
    f.setPixelSize(type::kMeta);
    label->setFont(f);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, kTextFaint);
    label->setPalette(pal);

    layout->addStretch();
    layout->addWidget(label);
    layout->addStretch();

    return page;
}

PanelFrame* MainWindow::makePanel(const QStringList& tabs, const QString& note) {
    auto* panel = new PanelFrame(tabs);
    for (int i = 0; i < tabs.size(); ++i) {
        panel->addPage(makePlaceholder(i == 0 ? note : tabs.at(i) + QStringLiteral(" panel")));
    }
    return panel;
}

QWidget* MainWindow::buildBody() {
    // Project | Viewer | Inspector
    bodySplit_ = new QSplitter(Qt::Horizontal);
    bodySplit_->setHandleWidth(metrics::kGutter);
    bodySplit_->setChildrenCollapsible(false);

    projectTabs_ = new PanelFrame({QStringLiteral("Project"),
                                    QStringLiteral("Pooled Media"),
                                    QStringLiteral("Comp Map")});
    projectPanel_ = new ProjectPanel;
    projectPanel_->setProject(&project_);
    pooledPanel_ = new PooledMediaPanel;
    pooledPanel_->setPool(&pool_);
    projectTabs_->addPage(projectPanel_);
    projectTabs_->addPage(pooledPanel_);
    projectTabs_->addPage(makePlaceholder(QStringLiteral("composition map")));

    // Effects/presets/CC in one panel; presets and CC say "nothing here" rather than
    // being blank.
    effectsTabs_ = new PanelFrame({QStringLiteral("Effects"), QStringLiteral("Presets"),
                                    QStringLiteral("CC")});
    // Three separate pages, not one panel + placeholders, so tabs can be dragged apart
    // and each still shows its own content/empty state.
    effectsPanel_ = new EffectsPanel;
    effectsPanel_->setTab(EffectsPanel::Tab::Effects);

    auto* presetsPanel = new EffectsPanel;
    presetsPanel->setTab(EffectsPanel::Tab::Presets);

    auto* ccPanel = new EffectsPanel;
    ccPanel->setTab(EffectsPanel::Tab::ColorCorrection);

    effectsTabs_->addPage(effectsPanel_);
    effectsTabs_->addPage(presetsPanel);
    effectsTabs_->addPage(ccPanel);

    // Stacked, not two docks: toolbar buttons choose which shows, since both visible
    // would halve an already-narrow column.
    leftDock_ = new QStackedWidget;
    leftDock_->addWidget(projectTabs_);
    leftDock_->addWidget(effectsTabs_);

    // All three, because any of them can end up holding effects once presets exist.
    for (EffectsPanel* panel : {effectsPanel_, presetsPanel, ccPanel}) {
        connect(panel, &EffectsPanel::effectActivated, this,
                [this](const std::string& id) { applyEffect(id); });
    }



    core::Composition& comp = project_.compositions().front();
    activeComp_ = comp.id;
    const QString compName = QString::fromStdString(comp.name);

    viewerTabs_ = new PanelFrame({QStringLiteral("Composition: %1").arg(compName),
                                  QStringLiteral("Footage"), QStringLiteral("Layer")});
    viewerPage_ = makeViewerPage(&viewerTimecode_, &viewport_);
    viewerTabs_->addPage(viewerPage_);
    viewerTabs_->addPage(makePlaceholder(QStringLiteral("footage viewer")));
    viewerTabs_->addPage(makePlaceholder(QStringLiteral("layer viewer")));

    // Transform and effect stack share one inspector rather than fighting for one dock.
    inspectorTabs_ = new PanelFrame({QStringLiteral("Inspector"), QStringLiteral("Align")});
    inspector_ = new InspectorView;
    inspector_->setComposition(&comp);
    inspectorTabs_->addPage(inspector_);
    if (viewport_ != nullptr) {
        // Viewer needs the same layer sizes as align math (media pool / text layout).
        viewport_->setLayerSizes(layerSizes());

        // Routed through the timeline so viewer and timeline never disagree on selection.
        connect(viewport_, &GpuViewport::layerPicked, this, [this](core::LayerId id) {
            if (timelinePanel_ != nullptr) {
                timelinePanel_->selectLayer(id);
                timelinePanel_->revealAnimated(id);
            }
            if (inspector_ != nullptr) {
                inspector_->setSelectedLayer(id);
            }
            updateAlignAvailability();
        });

        // One undo step per drag, bracketed the same way a timeline drag is.
        connect(viewport_, &GpuViewport::manipulationBegan, this,
                [this](const QString& label) { recordEdit(label); });
        connect(viewport_, &GpuViewport::layerTransformed, this, [this] {
            if (inspector_ != nullptr) {
                inspector_->update();
            }
            markDirty();
        });
        connect(viewport_, &GpuViewport::manipulationEnded, this, [this] {
            if (timelinePanel_ != nullptr) {
                timelinePanel_->refresh();
            }
        });
    }

    alignPanel_ = new AlignPanel;
    inspectorTabs_->addPage(alignPanel_);
    connect(alignPanel_, &AlignPanel::alignRequested, this,
            &MainWindow::alignSelectedLayer);
    connect(alignPanel_, &AlignPanel::distributeRequested, this,
            &MainWindow::distributeSelectedLayers);
    updateAlignAvailability();

    bodySplit_->addWidget(leftDock_);
    bodySplit_->addWidget(viewerTabs_);
    bodySplit_->addWidget(inspectorTabs_);

    // Visibility recomputed from the whole layout, not just the frame that changed.
    for (PanelFrame* frame :
         {projectTabs_, effectsTabs_, viewerTabs_, inspectorTabs_}) {
        connect(frame, &PanelFrame::tabsChanged, this,
                &MainWindow::updatePanelVisibility);
    }
    bodySplit_->setStretchFactor(1, 1);
    bodySplit_->setSizes({metrics::kProjectPanelW, 900, metrics::kInspectorPanelW});

    // Body over timeline
    outerSplit_ = new QSplitter(Qt::Vertical);
    outerSplit_->setHandleWidth(metrics::kGutter);
    outerSplit_->setChildrenCollapsible(false);

    auto* timeline = new PanelFrame({compName});
    timelineTabs_ = timeline;
    // Timeline tabs name open compositions, not panels; they don't move or accept drops.
    timeline->setTabsMovable(false);
    auto* timelinePanel = new TimelinePanel;
    timelinePanel_ = timelinePanel;
    timelinePanel->setComposition(&comp);
    timeline->addPage(timelinePanel);

    const double fps = comp.fps;
    connect(timelinePanel, &TimelinePanel::currentTimeChanged, this,
            [this, fps](double seconds) {
                if (viewerTimecode_ != nullptr) {
                    viewerTimecode_->setText(formatTimecode(seconds, fps));
                }
            });
    if (viewerTimecode_ != nullptr) {
        viewerTimecode_->setText(formatTimecode(3.14, fps));
    }

    connect(timelinePanel, &TimelinePanel::selectionChanged, inspector_,
            [this](core::LayerId id) {
                inspector_->setSelectedLayer(id);
                if (viewport_ != nullptr) {
                    viewport_->setSelectedLayer(
                        id == 0 ? std::optional<core::LayerId>{}
                                : std::optional<core::LayerId>{id});
                }
                updateAlignAvailability();
            });
    // Cmd-click changes the set without changing the primary, so align needs its own signal.
    connect(timelinePanel, &TimelinePanel::selectionSetChanged, this,
            &MainWindow::updateAlignAvailability);
    connect(timelinePanel, &TimelinePanel::currentTimeChanged, inspector_,
            [this](double seconds) { inspector_->setCurrentTime(seconds); });
    inspector_->setSelectedLayer(comp.layers.empty()
                                     ? std::optional<core::LayerId>{}
                                     : std::optional<core::LayerId>{comp.layers.front().id});
    inspector_->setCurrentTime(3.14);
    // Playback drives the timeline; everything else follows from there.
    playback_ = new Playback(this);
    playback_->configure(comp.duration, comp.fps);
    {
        // Device opened unconditionally, not lazily on first decode, so importing audio
        // into a silent session still gets a device.
        audioOut_ = audio::AudioOutput::create();
        if (audioOut_ != nullptr) {
            loadAudio();
            playback_->setAudio(audioOut_.get());
        }
    }
    playback_->seek(3.14);
    connect(playback_, &Playback::timeChanged, timelinePanel,
            &TimelinePanel::setCurrentTime);
    // Ignored while playing, to avoid fighting the transport's own clock; only applies
    // when paused.
    connect(timelinePanel, &TimelinePanel::currentTimeChanged, this,
            [this](double seconds) {
                if (playback_->playing()) {
                    return;
                }
                playback_->seek(seconds);
            });
    connect(playback_, &Playback::playingChanged, this,
            [this](bool) { updateStatus(); });

    auto* playPause = new QAction(QStringLiteral("Play/Pause"), this);
    playPause->setShortcut(Qt::Key_Space);
    playPause->setShortcutContext(Qt::ApplicationShortcut);
    connect(playPause, &QAction::triggered, playback_, &Playback::togglePlay);
    addAction(playPause);

    connect(inspector_, &InspectorView::propertyEdited, timelinePanel,
            &TimelinePanel::refresh);
    connect(inspector_, &InspectorView::editBegan, this, &MainWindow::beginEdit);
    connect(inspector_, &InspectorView::editEnded, this, &MainWindow::endEdit);

    // Dragging a bar is one undo step, and the viewer has to follow because a layer's
    // in and out points decide whether it is on screen at all.
    connect(timelinePanel, &TimelinePanel::editBegan, this, &MainWindow::beginEdit);
    connect(timelinePanel, &TimelinePanel::editEnded, this, &MainWindow::endEdit);
    connect(timelinePanel, &TimelinePanel::layersChanged, this, [this] {
        if (viewport_ != nullptr) {
            viewport_->update();
        }
        // Locking arrives here (not via selectionChanged), so align buttons still grey out.
        updateAlignAvailability();
        // Mixer holds its own copy of layer times; rebuilt on every drag step, not just
        // at the end, so audio stays in sync while dragging.
        rebuildMix();
    });
    // View grows the composition on release (it knows when the drag ended); the window
    // still needs to know since duration affects looping and the status bar.
    connect(timelinePanel, &TimelinePanel::compositionResized, this,
            [this](double) { noteCompositionGrew(); });
    // Toggling a speaker republishes the mix; undoable via editBegan/editEnded in the view.
    connect(timelinePanel, &TimelinePanel::audioChanged, this, [this] {
        markDirty();
    });

    // Selection, Rotation and Anchor act in the viewer; other tools still swallow clicks
    // there rather than falling back to Selection.
    connect(toolBar_, &EditorToolBar::toolSelected, this, [this](int index) {
        if (viewport_ != nullptr) {
            viewport_->setTool(EditorToolBar::toolAt(index));
        }
    });

    connect(toolBar_, &EditorToolBar::snappingToggled, timelinePanel,
            &TimelinePanel::setSnapping);

    connect(toolBar_, &EditorToolBar::featureTriggered, this, [this](int index) {
        if (index == 0) {
            runBeatAnalyzer();
        } else {
            // Audio Studio is specified but not built. See NOTEBOOK F6.
            statusBar()->showMessage(
                QStringLiteral("Audio Studio is not built yet"), 4000);
        }
    });

    // Collapsed to a menu rather than a row of names: Ruby has one layout, unlike AE.
    connect(toolBar_, &EditorToolBar::workspaceMenuRequested, this,
            [this](const QPoint& at) {
                QMenu menu(this);
                QAction* def = menu.addAction(QStringLiteral("Default"));
                def->setCheckable(true);
                def->setChecked(true);
                menu.addSeparator();
                addPending(&menu, {QStringLiteral("Save Workspace..."),
                                   QStringLiteral("Reset Workspace")});
                menu.exec(at);
            });

    // The two panel buttons choose what the left column shows.
    connect(toolBar_, &EditorToolBar::panelSelected, this, [this](int index) {
        if (leftDock_ != nullptr) {
            leftDock_->setCurrentIndex(index);
        }
    });

    connect(this, &MainWindow::mediaImported, projectPanel_, &ProjectPanel::refresh);
    connect(projectPanel_, &ProjectPanel::compositionActivated, this,
            &MainWindow::setActiveComposition);
    connect(timeline, &PanelFrame::currentChanged, this, [this](int index) {
        const auto& comps = project_.compositions();
        if (index >= 0 && index < static_cast<int>(comps.size())) {
            setActiveComposition(comps[static_cast<std::size_t>(index)].id);
        }
    });
    connect(projectPanel_, &ProjectPanel::mediaActivated, this,
            &MainWindow::addMediaToComposition);
    connect(projectPanel_, &ProjectPanel::newCompositionRequested, this,
            &MainWindow::newComposition);
    connect(projectPanel_, &ProjectPanel::compositionFromMediaRequested, this,
            &MainWindow::compositionFromMedia);
    connect(projectPanel_, &ProjectPanel::deleteRequested, this,
            &MainWindow::deleteProjectItem);
    connect(timelinePanel, &TimelinePanel::mediaDropped, this,
            &MainWindow::dropMediaIntoComposition);
    connect(timelinePanel, &TimelinePanel::layerContextMenuRequested, this,
            &MainWindow::showLayerContextMenu);
    // Selects the drop target layer first, then applies; same care the right-click menu
    // takes to not act on the wrong selection.
    connect(timelinePanel, &TimelinePanel::effectDropped, this,
            [this](core::LayerId layer, const std::string& id) {
                if (timelinePanel_ != nullptr) {
                    timelinePanel_->selectLayer(layer);
                }
                applyEffect(id);
            });

    connect(timelinePanel, &TimelinePanel::effectContextMenuRequested, this,
            [this](int index, const QPoint& at) {
                QMenu menu(this);
                menu.addAction(QStringLiteral("Remove Effect"), this,
                               [this, index] { removeEffect(index); });
                menu.addAction(QStringLiteral("Remove All Effects"), this,
                               &MainWindow::removeAllEffects);
                menu.exec(at);
            });

    if (viewport_ != nullptr && gpu_ != nullptr) {
        viewport_->setDevice(gpu_);
        viewport_->setProject(&project_);
        viewport_->setComposition(&comp);
        viewport_->setCurrentTime(3.14);
        connect(timelinePanel, &TimelinePanel::currentTimeChanged, viewport_,
                &GpuViewport::setCurrentTime);
        // An edit changes what the frame looks like, so the viewer redraws too.
        connect(inspector_, &InspectorView::propertyEdited, viewport_,
                [this] { viewport_->update(); });
    }

    outerSplit_->addWidget(bodySplit_);
    outerSplit_->addWidget(timeline);
    outerSplit_->setStretchFactor(0, 1);

    // Design allots 280px of *tracks*; panel also carries a 26px tab strip and 26px
    // sub-toolbar, so total height must be 332 for tracks to actually get 280.
    constexpr int kTimelineChrome = metrics::kTabStripH + metrics::kSubToolbarH;
    constexpr int kTimelineTracks = 280;
    outerSplit_->setSizes({428, kTimelineTracks + kTimelineChrome});

    outerSplit_->setAutoFillBackground(true);
    QPalette pal = outerSplit_->palette();
    pal.setColor(QPalette::Window, kGutter);
    outerSplit_->setPalette(pal);

    return outerSplit_;
}

}  // namespace ruby::ui
