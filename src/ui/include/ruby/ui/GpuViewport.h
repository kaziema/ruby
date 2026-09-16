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

// Widget backed by a real GPU swapchain rather than Qt's painter. WA_PaintOnScreen +
// null paintEngine keep Qt out of the way; WA_NativeWindow guarantees a native window
// handle to hand to Dawn.
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
    // A layer was clicked. The window turns this into the real selection so timeline
    // and viewer agree.
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

    // Rasterises and uploads text layers that need it; cached on text+style, so
    // scrubbing a static caption re-uploads nothing.
    void refreshTextTextures();

    // Widget positions are logical points; the swapchain (and everything the compositor
    // computed) is in physical pixels. Convert once here — nowhere else should.
    [[nodiscard]] QPointF toSurface(const QPointF& widgetPos) const;
    [[nodiscard]] engine::FrameFit surfaceFit() const;

    // Inverse-transforms into each layer's unit square rather than using a bounding box,
    // since a rotated layer's box has clickable corners that aren't the layer.
    [[nodiscard]] std::optional<core::LayerId> layerAt(const QPointF& widgetPos) const;

    // Handles for the selected layer, in widget pixels.
    [[nodiscard]] engine::Compositor::Overlay buildOverlay() const;

    struct TextTexture {
        gpu::TextureHandle texture;
        int width = 0;
        int height = 0;
        std::size_t key = 0;  // hash of everything that changes the picture
    };

    // What a drag is doing. Fixed at press; can't change meaning mid-drag.
    enum class Grab { None, Move, Rotate, Anchor, Scale };

    ToolIcon tool_ = ToolIcon::Selection;
    std::optional<core::LayerId> selected_;
    core::SizeOf layerSizes_;

    Grab grab_ = Grab::None;
    QPointF grabStart_;
    core::Value grabOriginal_;   // the property's value when the drag began
    double grabAngle_ = 0.0;     // for Rotate: the angle from the anchor at press

    // For Scale: grabbed handle (unit-box coords) and the press-time box-to-widget
    // transform, held fixed for the drag.
    double grabHandleU_ = 0.0;
    double grabHandleV_ = 0.0;
    bool grabAffectsX_ = true;
    bool grabAffectsY_ = true;
    core::Transform2D grabBack_;

    gpu::GpuDevice* device_ = nullptr;
    gpu::SurfaceHandle surface_;
    std::unique_ptr<engine::Compositor> compositor_;

public:
    // Which frames in a range are already cached in RAM. `contains` must not be a
    // lookup — drawing the cache bar must not itself alter cache state.
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
