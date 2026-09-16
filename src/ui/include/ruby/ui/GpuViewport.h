#pragma once

#include <QWidget>

#include <map>
#include <memory>

#include "ruby/core/Document.h"
#include "ruby/engine/Compositor.h"
#include "ruby/core/Transform.h"
#include "ruby/gpu/GpuDevice.h"
#include "ruby/ui/ToolIcons.h"

#include <optional>

namespace ruby::ui {

// A widget backed by a real GPU swapchain rather than by Qt's painter.
//
// Qt normally draws every widget into its own backing store and composites the result.
// That fights a swapchain, which wants to own the window's pixels. WA_PaintOnScreen plus
// a null paintEngine tells Qt to stay out of the way, and WA_NativeWindow guarantees an
// actual NSView to hand to Dawn.
class GpuViewport : public QWidget {
    Q_OBJECT

public:
    explicit GpuViewport(QWidget* parent = nullptr);

    // The device is owned elsewhere and outlives this widget.
    void setDevice(gpu::GpuDevice* device);

    // The composition to draw, and where the playhead is. Both may be null or stale;
    // the viewport just shows an empty frame in that case.
    void setProject(const core::Project* project);
    void setComposition(const core::Composition* comp);
    void setCurrentTime(double seconds);

    // Which tool a click in here means. Set from the toolbar.
    void setTool(ToolIcon tool);

    // Which layer is selected, so its handles are drawn. The timeline owns the selection;
    // this only mirrors it.
    void setSelectedLayer(std::optional<core::LayerId> layer);

    // How big a layer is, before its own scale. Supplied by the window, which is the only
    // thing that can measure text and read the media pool.
    void setLayerSizes(core::SizeOf sizes);

signals:
    // A layer was clicked in the picture. The window turns this into a real selection, so
    // the timeline and the viewer never hold two different opinions about it.
    void layerPicked(core::LayerId layer);

    // A drag began and ended, bracketing one undo step, and something changed in between.
    void manipulationBegan(const QString& label);
    void manipulationEnded();
    void layerTransformed();

public:

protected:
    // Returning null is what disables Qt's own painting for this widget.
    [[nodiscard]] QPaintEngine* paintEngine() const override { return nullptr; }

    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    void ensureSurface();
    void configureSurface();

    // Rasterises every text layer that needs it and uploads the results. Cached on the
    // layer's own text and style, so a comp full of captions costs one raster each rather
    // than one per frame; scrubbing a static caption re-uploads nothing.
    void refreshTextTextures();

    // Everything that has to agree with what was DRAWN works in surface pixels.
    //
    // This widget has two coordinate systems and they differ by the display's scale
    // factor: Qt hands out mouse positions and widget sizes in logical points, while the
    // swapchain, and therefore everything the compositor computed, is in physical pixels.
    // Mixing them put the handles at half scale in the corner and made every click land on
    // the wrong part of the picture.
    //
    // So there is exactly one conversion, here, and no site below computes its own.
    [[nodiscard]] QPointF toSurface(const QPointF& widgetPos) const;
    [[nodiscard]] engine::FrameFit surfaceFit() const;

    // The topmost layer whose quad contains this point, or nothing.
    //
    // Works by putting the point back through each layer's transform rather than by
    // comparing against a bounding box: a rotated layer's box covers corners that are not
    // the layer, and clicking one of those would select something you are not pointing at.
    [[nodiscard]] std::optional<core::LayerId> layerAt(const QPointF& widgetPos) const;

    // Handles for the selected layer, in widget pixels.
    [[nodiscard]] engine::Compositor::Overlay buildOverlay() const;

    struct TextTexture {
        gpu::TextureHandle texture;
        int width = 0;
        int height = 0;
        std::size_t key = 0;  // hash of everything that changes the picture
    };

    // What a drag is doing. Decided on press and fixed until release, because a gesture
    // that changes meaning halfway through is a gesture nobody can aim.
    enum class Grab { None, Move, Rotate, Anchor, Scale };

    ToolIcon tool_ = ToolIcon::Selection;
    std::optional<core::LayerId> selected_;
    core::SizeOf layerSizes_;

    Grab grab_ = Grab::None;
    QPointF grabStart_;
    core::Value grabOriginal_;   // the property's value when the drag began
    double grabAngle_ = 0.0;     // for Rotate: the angle from the anchor at press

    // For Scale: which handle was grabbed (in the same unit-box coordinates the handles
    // are drawn in) and the box-to-widget transform as it stood at press, held fixed for
    // the whole drag so the math below doesn't chase a target that is itself moving.
    double grabHandleU_ = 0.0;
    double grabHandleV_ = 0.0;
    bool grabAffectsX_ = true;
    bool grabAffectsY_ = true;
    core::Transform2D grabBack_;

    gpu::GpuDevice* device_ = nullptr;
    gpu::SurfaceHandle surface_;
    std::unique_ptr<engine::Compositor> compositor_;

public:
    // Which frames in a range are already held in RAM.
    //
    // Answered by building each frame's graph and asking the cache whether it holds every
    // layer that frame needs. Building a graph is pure and cheap, and `contains` is
    // deliberately not a lookup: drawing the cache bar must not reorder the cache, or the
    // act of looking at what is ready changes what is ready.
    [[nodiscard]] std::vector<std::pair<double, bool>> cachedFrames(double from, double to)
        const;

    // How full the preview cache is. Empty when there is no compositor.
    [[nodiscard]] engine::FrameCache::Stats cacheStats() const {
        return compositor_ != nullptr ? compositor_->cache().stats()
                                      : engine::FrameCache::Stats{};
    }

private:
    const core::Project* project_ = nullptr;
    const core::Composition* comp_ = nullptr;
    double currentTime_ = 0.0;
    std::map<core::LayerId, TextTexture> textTextures_;
};

}  // namespace ruby::ui
