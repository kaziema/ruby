#pragma once

#include <QMainWindow>
#include <QString>

class QAction;
class QCloseEvent;

#include "ruby/core/Document.h"
#include "ruby/core/Transform.h"
#include "ruby/ui/AlignPanel.h"
#include <map>
#include <optional>

#include "ruby/gpu/GpuDevice.h"
#include "ruby/audio/AudioOutput.h"
#include "ruby/io/History.h"
#include "ruby/io/MediaPool.h"
#include "ruby/media/PeakCache.h"
#include "ruby/media/AudioDecoder.h"

class QLabel;
class QSplitter;
class QStackedWidget;

namespace ruby::ui {

class EditorToolBar;
class GpuViewport;
class Playback;
class PanelFrame;
class ProjectPanel;
class PooledMediaPanel;
class EffectsPanel;
class StatusReadout;
class TimelinePanel;
class InspectorView;
class PanelFrame;

// The main editor layout:
//   menu bar -> tool bar (30px) -> [Project 250 | Viewer flex | Inspector 268]
//                               -> timeline (full width)
// Panels sit in a 2px gutter grid. Splitter handles are that gutter, so panel
// edges are draggable and the widths are defaults rather than constraints.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // Owned by the application, not this window. Null is legal: viewport stays blank,
    // which also lets headless tests open a menu without a graphics stack.
    explicit MainWindow(gpu::GpuDevice* device = nullptr, QWidget* parent = nullptr);

    [[nodiscard]] const core::Project& project() const noexcept { return project_; }

    // Layer size before its own scale. Derived from the media pool / text layout here
    // rather than decoded frames, but agrees wherever the align panel needs it to.
    [[nodiscard]] core::SizeOf layerSizes();

    // Moves the selected layer so one of its edges or centres meets the composition's.
    void alignSelectedLayer(AlignPanel::Align edge, AlignPanel::Target target);
    void distributeSelectedLayers(AlignPanel::Align axis);

    // The selected layers that have a picture and are not locked.
    [[nodiscard]] std::vector<core::LayerId> alignableSelection();

    // Moves a layer by composition pixels, converted into the parent's space and into
    // Position's percentage units. Returns whether it actually moved.
    [[nodiscard]] bool nudgeLayerBy(core::Layer& layer, double dx, double dy,
                                    double seconds, const core::SizeOf& sizes);

    // Turns the align buttons on or off for whatever is selected now.
    void updateAlignAvailability();

    // Reads the viewer's cache and redraws the bar under the ruler.
    void refreshCacheBar();

signals:
    // The pool changed. The project panel listens; nothing else needs to yet.
    void mediaImported();

public slots:
    void splitLayerAtPlayhead();
    void toggleSelectedLayerProperties();
    void revealSelectedLayerAudioLevel();
    void undo();
    void redo();
    void beginEdit(const QString& label);
    void endEdit();
    void newProject();
    void openProject();

    // Opens a specific file, skipping the dialog. Public because the application opens a
    // project named on the command line before the window is shown.
    void openProject(const QString& path);
    bool saveProject(bool forcePrompt);
    void importMedia();
    void newComposition();
    void compositionSettings();

    // Edit menu. Selection is one layer at a time, so these all act on that one.
    void deleteLayer();
    void duplicateLayer();
    void cutLayer();
    void copyLayer();
    void pasteLayer();
    void deselectAll();

    // Effect menu. Everything here acts on the selected layer.
    void applyEffect(const std::string& effectId);
    void removeAllEffects();
    void removeEffect(int index);

    // Beat Analyzer: pick a lane, run the detector over this composition's audio, and put
    // the result on the timeline.
    void runBeatAnalyzer();

    // Project panel footer.
    void compositionFromMedia(core::MediaId media);
    void deleteProjectItem(bool isComposition, std::uint64_t id);

    // Layer > New. Lands above the selected layer (as AE does), not atop the whole stack.
    void newSolidLayer();
    void newNullLayer();

    // Also the editor: double-clicking a text layer's name reopens it seeded.
    void newTextLayer();
    void editTextLayer();

    // Shared by both: place the layer, span the composition, select it, refresh.
    core::Layer* createLayer(const QString& undoLabel, const std::string& name,
                             core::LayerKind kind);

    // Right-click menu; reuses the Edit/Layer menu QActions so shortcuts can't drift.
    void showLayerContextMenu(const QPoint& globalPos);

    // Shared by delete and cut. Picks the next sensible selection and refreshes.
    void removeSelectedLayer(const QString& undoLabel);

    // Called when growToFit changes duration; rescaling every timeline bar silently
    // would read as a glitch otherwise.
    void noteCompositionGrew();

    // Decodes a clip's audio and writes its peak pyramid to the cache. Failed covers
    // both "no audio" and "unreadable".
    media::ConformState conformAudio(const QString& path);

    // Rebuilds the mix from already-decoded buffers; cheap enough to run on every drag
    // move. loadAudio does the expensive decode/peaks/rhythm work, then calls this.
    void rebuildMix();
    void addMediaToComposition(core::MediaId id);
    void dropMediaIntoComposition(core::MediaId media, double seconds, int layerIndex);
    void setActiveComposition(core::CompId id);

protected:
    // Override (not a signal) so closing with unsaved work can be intercepted.
    void closeEvent(QCloseEvent* e) override;

private:
    void buildMenus();
    void updateStatus();
    void updateReadouts();
    void loadAudio();
    bool confirmDiscard();
    void markDirty();
    void markClean();
    void updateTitle();
    void recordEdit(const QString& label);
    void refreshUndoActions();
    void afterDocumentReplaced();
    [[nodiscard]] core::Layer* selectedLayer();
    void nudgeLayerEdge(bool inPoint, bool trim);
    void jumpToKeyframe(bool forward);
    void refreshCompositionTabs();
    [[nodiscard]] core::Composition* activeComposition();
    QWidget* buildBody();

    static PanelFrame* makePanel(const QStringList& tabs, const QString& note);
    static QWidget* makePlaceholder(const QString& note);


    // Layer clipboard, by value. Media/precomp refs are project-level ids, so pasting
    // into a different composition still resolves.
    std::optional<core::Layer> clipboard_;

    // App-level (not project-level): every clip ever imported, across all projects.
    io::MediaPool pool_;
    QString poolPath_;

    // Peak caches live beside the pool, one file per clip, keyed on path+size+mtime.
    QString peaksDir_;

    core::Project project_;  // TEMPORARY demo content
    // Keyed so shared clips decode once. Map, not vector: the mixer holds raw pointers
    // into these entries, which a reallocating vector would invalidate under the audio
    // thread. Never erased mid-session for the same reason.
    std::map<core::MediaId, media::AudioBuffer> audio_;

    // Peak pyramids for drawing, shared by every layer using the clip rather than
    // duplicated per-layer.
    std::map<core::MediaId, media::PeakPyramid> peaks_;

    // Last clip analyzed for rhythm; expensive, so it must not re-run on every nudge.
    std::optional<core::MediaId> analyzedRhythmFor_;
    std::unique_ptr<audio::AudioOutput> audioOut_;
    QString rhythmNote_;
    EditorToolBar* toolBar_ = nullptr;
    QLabel* viewerTimecode_ = nullptr;
    InspectorView* inspector_ = nullptr;
    GpuViewport* viewport_ = nullptr;
    Playback* playback_ = nullptr;
    ProjectPanel* projectPanel_ = nullptr;
    PooledMediaPanel* pooledPanel_ = nullptr;
    EffectsPanel* effectsPanel_ = nullptr;
    AlignPanel* alignPanel_ = nullptr;
    QStackedWidget* leftDock_ = nullptr;
    StatusReadout* readout_ = nullptr;
    TimelinePanel* timelinePanel_ = nullptr;
    PanelFrame* timelineTabs_ = nullptr;
    PanelFrame* viewerTabs_ = nullptr;
    PanelFrame* projectTabs_ = nullptr;
    PanelFrame* effectsTabs_ = nullptr;
    PanelFrame* inspectorTabs_ = nullptr;

    // The composition viewer's page, so its tab can be renamed wherever it ends up.
    QWidget* viewerPage_ = nullptr;

    // Recomputed rather than toggled per event: a tab move is a removal + insertion, and
    // the in-between state shouldn't be visible.
    void updatePanelVisibility();
    core::CompId activeComp_ = 0;
    QString projectPath_;
    bool dirty_ = false;
    io::History history_;
    QAction* undoAction_ = nullptr;
    QAction* cutAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* pasteAction_ = nullptr;
    QAction* duplicateAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* splitAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    gpu::GpuDevice* gpu_ = nullptr;
    QSplitter* bodySplit_ = nullptr;
    QSplitter* outerSplit_ = nullptr;
};

}  // namespace ruby::ui
