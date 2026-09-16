#include "ruby/ui/GpuViewport.h"

#include "ruby/core/Expressions.h"
#include "ruby/ui/TextRaster.h"

#include <cmath>

#include <QGuiApplication>
#include <QResizeEvent>
#include <QShowEvent>
#include <algorithm>

namespace ruby::ui {

GpuViewport::GpuViewport(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(16, 16);
    // Mouse tracking so a tool can show its cursor before anything is pressed.
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
}

void GpuViewport::setDevice(gpu::GpuDevice* device) {
    device_ = device;
    surface_.reset();
    ensureSurface();
    update();
}

void GpuViewport::setProject(const core::Project* project) {
    project_ = project;
    update();
}

void GpuViewport::setComposition(const core::Composition* comp) {
    comp_ = comp;
    update();
}

void GpuViewport::setCurrentTime(double seconds) {
    currentTime_ = seconds;
    update();
}

void GpuViewport::ensureSurface() {
    if (device_ == nullptr || surface_ != nullptr) {
        return;
    }
    // winId() forces window creation, so this can't run in the constructor. Some
    // platforms (offscreen plugin) never produce one; a blank viewport is fine there.
    const WId handle = winId();
    if (handle == 0) {
        return;
    }
    surface_ = device_->create_surface(reinterpret_cast<void*>(handle));
    configureSurface();

    // The pipeline has to be built against whatever format the swapchain gave us.
    if (surface_ != nullptr) {
        compositor_ = std::make_unique<engine::Compositor>(*device_, surface_->format());
    }
}

void GpuViewport::configureSurface() {
    if (surface_ == nullptr) {
        return;
    }
    // Swapchains are sized in physical pixels, widgets in logical ones.
    const double dpr = devicePixelRatioF();
    const auto w = static_cast<std::uint32_t>(std::max(1.0, width() * dpr));
    const auto h = static_cast<std::uint32_t>(std::max(1.0, height() * dpr));
    surface_->configure(w, h);
}

void GpuViewport::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    ensureSurface();
}

void GpuViewport::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    configureSurface();
}

// Rasterises text layers and uploads them. Keyed on a hash of everything that affects
// pixels (text, font, size, color, alignment, tracking, line height) — not position or
// opacity, which the compositor handles without a re-rasterise.
void GpuViewport::refreshTextTextures() {
    if (device_ == nullptr || comp_ == nullptr) {
        textTextures_.clear();
        return;
    }

    std::map<core::LayerId, TextTexture> kept;
    for (const core::Layer& layer : comp_->layers) {
        if (layer.kind != core::LayerKind::Text || layer.text.empty()) {
            continue;
        }

        std::size_t key = std::hash<std::string>{}(layer.text);
        const auto mix = [&key](std::size_t v) { key = key * 1099511628211ULL ^ v; };
        mix(std::hash<std::string>{}(layer.fontFamily));
        mix(std::hash<double>{}(layer.fontSize));
        mix(std::hash<double>{}(layer.tracking));
        mix(std::hash<double>{}(layer.lineHeight));
        mix(std::hash<double>{}(layer.strokeWidth));
        mix(static_cast<std::size_t>(layer.textAlign));
        for (int i = 0; i < 4; ++i) {
            mix(std::hash<double>{}(layer.textColor.c[static_cast<std::size_t>(i)]));
            mix(std::hash<double>{}(layer.strokeColor.c[static_cast<std::size_t>(i)]));
        }

        if (const auto existing = textTextures_.find(layer.id);
            existing != textTextures_.end() && existing->second.key == key) {
            kept.emplace(layer.id, existing->second);
            continue;
        }

        // Rasterised at composition resolution, not a fixed size then scaled — scaled
        // type is mush.
        const TextRaster raster = rasteriseText(layer, 1.0);
        if (!raster.valid()) {
            continue;
        }

        gpu::TextureDesc desc;
        desc.width = raster.image.width();
        desc.height = raster.image.height();
        desc.format = gpu::TextureFormat::RGBA8Unorm;
        desc.usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst;
        desc.debug_label = "text";

        TextTexture made;
        made.texture = device_->create_texture(desc);
        made.width = desc.width;
        made.height = desc.height;
        made.key = key;
        if (made.texture == nullptr) {
            continue;
        }
        device_->write_texture(made.texture, raster.image.constBits(),
                               static_cast<std::size_t>(raster.image.sizeInBytes()),
                               static_cast<std::uint32_t>(raster.image.bytesPerLine()));
        kept.emplace(layer.id, std::move(made));
    }

    // Anything not rebuilt this pass belongs to a removed/non-text layer; drop it.
    textTextures_ = std::move(kept);
}

void GpuViewport::paintEvent(QPaintEvent*) {
    ensureSurface();
    if (device_ == nullptr || surface_ == nullptr) {
        return;
    }

    gpu::TextureHandle backbuffer = surface_->acquire();
    if (backbuffer == nullptr) {
        return;  // mid-resize or occluded; skipping a frame is correct here
    }

    if (compositor_ != nullptr && comp_ != nullptr && project_ != nullptr) {
        refreshTextTextures();

        engine::Compositor::ExternalTextures external;
        engine::ExternalKeys keys;
        for (const auto& [id, text] : textTextures_) {
            external.emplace(id, engine::Compositor::External{text.texture, text.width,
                                                              text.height});
            // Same key the re-rasterise check above uses: unchanged key means unchanged
            // raster.
            keys.emplace(id, static_cast<std::uint64_t>(text.key));
        }
        const engine::Compositor::Overlay overlay = buildOverlay();
        compositor_->render(*project_, *comp_, currentTime_, backbuffer, &external, &keys,
                            overlay.empty() ? nullptr : &overlay);
    } else {
        auto commands = device_->begin_commands("viewport");
        commands->begin_pass(backbuffer, 0.008f, 0.008f, 0.008f, 1.0f);
        commands->end_pass();
        device_->submit(std::move(commands));
    }

    surface_->present();
}

// --- direct manipulation ------------------------------------------------------

void GpuViewport::setTool(ToolIcon tool) {
    if (tool_ == tool) {
        return;
    }
    tool_ = tool;
    // A tool change cancels a drag rather than letting it finish under new rules.
    grab_ = Grab::None;
    switch (tool_) {
        case ToolIcon::Hand: setCursor(Qt::OpenHandCursor); break;
        case ToolIcon::Rotation:
        case ToolIcon::Anchor: setCursor(Qt::CrossCursor); break;
        default: setCursor(Qt::ArrowCursor); break;
    }
    update();
}

void GpuViewport::setSelectedLayer(std::optional<core::LayerId> layer) {
    if (selected_ == layer) {
        return;
    }
    selected_ = layer;
    update();
}

void GpuViewport::setLayerSizes(core::SizeOf sizes) { layerSizes_ = std::move(sizes); }

// The transform taking a layer's own unit quad all the way to widget pixels.
namespace {

core::Transform2D unitToWidget(const core::Composition& comp, const core::Layer& layer,
                               double seconds, const core::SizeOf& sizes,
                               const engine::FrameFit& fit) {
    const core::LayerSize size = sizes ? sizes(layer) : core::LayerSize{};
    const core::TimeContext ctx = comp.timeContext();
    // Must match the compositor's exact chain order, or handles land off the layer.
    const core::Transform2D unitToLayer =
        core::Transform2D::translate(-0.5, -0.5)
            .then(core::Transform2D::scale(size.width, size.height));
    const core::Transform2D toComp = core::resolvedTransform(
        comp, layer, seconds, ctx, static_cast<double>(comp.width),
        static_cast<double>(comp.height), sizes);
    const core::Transform2D compToWidget =
        core::Transform2D::scale(fit.scale, fit.scale)
            .then(core::Transform2D::translate(fit.x, fit.y));
    return unitToLayer.then(toComp).then(compToWidget);
}

// The eight resize handles, in unit-box coords (0,0 top-left, 1,1 bottom-right).
// One table drives both drawing and hit-testing, so they can't disagree.
struct HandleSpec {
    double u, v;
    bool affectsX, affectsY;  // which axis of scale dragging this handle changes
    double markerSize;
};

constexpr HandleSpec kHandles[8] = {
    {0.0, 0.0, true, true, 7.0},    // top-left
    {1.0, 0.0, true, true, 7.0},    // top-right
    {1.0, 1.0, true, true, 7.0},    // bottom-right
    {0.0, 1.0, true, true, 7.0},    // bottom-left
    {0.5, 0.0, false, true, 5.0},   // top edge
    {0.5, 1.0, false, true, 5.0},   // bottom edge
    {0.0, 0.5, true, false, 5.0},   // left edge
    {1.0, 0.5, true, false, 5.0},   // right edge
};

// Nearest handle within hit radius, or none. Radius is a bit larger than the drawn
// marker, like any small hit target.
std::optional<HandleSpec> hitHandle(const core::Transform2D& toWidget,
                                    const QPointF& surfacePos) {
    constexpr double kHitRadius = 9.0;
    std::optional<HandleSpec> best;
    double bestDist = kHitRadius;
    for (const HandleSpec& h : kHandles) {
        const double px = toWidget.applyX(h.u, h.v);
        const double py = toWidget.applyY(h.u, h.v);
        const double dx = px - surfacePos.x();
        const double dy = py - surfacePos.y();
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= bestDist) {
            bestDist = dist;
            best = h;
        }
    }
    return best;
}

}  // namespace

QPointF GpuViewport::toSurface(const QPointF& widgetPos) const {
    const double dpr = devicePixelRatioF();
    return QPointF(widgetPos.x() * dpr, widgetPos.y() * dpr);
}

engine::FrameFit GpuViewport::surfaceFit() const {
    if (comp_ == nullptr) {
        return {};
    }
    // Same numbers configureSurface gave the swapchain, which the compositor drew with.
    const double dpr = devicePixelRatioF();
    return engine::Compositor::fitFor(*comp_, static_cast<double>(width()) * dpr,
                                      static_cast<double>(height()) * dpr);
}

std::optional<core::LayerId> GpuViewport::layerAt(const QPointF& surfacePos) const {
    if (comp_ == nullptr) {
        return std::nullopt;
    }
    const engine::FrameFit fit = surfaceFit();
    const core::TimeContext ctx = comp_->timeContext();

    // Topmost first (list order), i.e. reverse draw order.
    for (const core::Layer& layer : comp_->layers) {
        if (!layer.enabled || layer.locked || layer.kind == core::LayerKind::Audio ||
            layer.kind == core::LayerKind::Null) {
            continue;
        }
        const double in = core::to_seconds(layer.inPoint, ctx);
        const double out = core::to_seconds(layer.outPoint, ctx);
        if (currentTime_ < in || currentTime_ >= out) {
            continue;  // not on screen, so not clickable
        }
        const core::Transform2D back =
            unitToWidget(*comp_, layer, currentTime_, layerSizes_, fit).inverse();
        const double u = back.applyX(surfacePos.x(), surfacePos.y());
        const double v = back.applyY(surfacePos.x(), surfacePos.y());
        if (u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0) {
            return layer.id;
        }
    }
    return std::nullopt;
}

engine::Compositor::Overlay GpuViewport::buildOverlay() const {
    engine::Compositor::Overlay out;
    if (comp_ == nullptr || !selected_.has_value()) {
        return out;
    }
    const core::Layer* layer = comp_->find(*selected_);
    if (layer == nullptr || layer->kind == core::LayerKind::Audio) {
        return out;
    }
    const engine::FrameFit fit = surfaceFit();
    const core::Transform2D toWidget =
        unitToWidget(*comp_, *layer, currentTime_, layerSizes_, fit);

    const auto at = [&toWidget](double u, double v) {
        return QPointF(toWidget.applyX(u, v), toWidget.applyY(u, v));
    };
    // Square centered on a point, upright in screen space (not rotated with the layer).
    const auto marker = [&out](const QPointF& p, double size, float r, float g, float b) {
        const double h = size * 0.5;
        out.push_back({core::Transform2D::scale(size, size)
                           .then(core::Transform2D::translate(p.x() - h, p.y() - h)),
                       r, g, b, 1.0f});
    };
    // A one pixel line between two points, as a scaled and rotated unit quad.
    const auto line = [&out](const QPointF& a, const QPointF& b) {
        const double dx = b.x() - a.x();
        const double dy = b.y() - a.y();
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.5) {
            return;
        }
        const double deg = std::atan2(dy, dx) * 180.0 / M_PI;
        out.push_back({core::Transform2D::translate(0.0, -0.5)
                           .then(core::Transform2D::scale(len, 1.0))
                           .then(core::Transform2D::rotate(deg))
                           .then(core::Transform2D::translate(a.x(), a.y())),
                       0.29f, 0.62f, 0.85f, 0.9f});
    };

    const QPointF tl = at(0.0, 0.0);
    const QPointF tr = at(1.0, 0.0);
    const QPointF br = at(1.0, 1.0);
    const QPointF bl = at(0.0, 1.0);
    line(tl, tr);
    line(tr, br);
    line(br, bl);
    line(bl, tl);

    // Same handle table hitHandle() uses, so drawn handles match what's grabbable.
    for (const HandleSpec& h : kHandles) {
        marker(at(h.u, h.v), h.markerSize, 0.9f, 0.9f, 0.9f);
    }

    // Anchor/pivot point. Always drawn, not just with the Anchor tool, since an unseen
    // pivot is the usual cause of a surprising rotation.
    const core::TimeContext ctx = comp_->timeContext();
    const core::Property* ap = layer->find("anchor_point");
    const core::Value a = ap != nullptr ? core::evaluate(*layer, *ap, currentTime_, ctx)
                                        : core::Value::vec2(0.0, 0.0);
    const QPointF pivot = at(0.5 + a.c[0] / 100.0, 0.5 + a.c[1] / 100.0);
    marker(pivot, 9.0, 0.29f, 0.62f, 0.85f);
    marker(pivot, 3.0, 1.0f, 1.0f, 1.0f);
    return out;
}

// --- the drags ----------------------------------------------------------------

namespace {

// A layer's pivot in composition pixels: the point rotation and scale turn around.
QPointF pivotInComp(const core::Composition& comp, const core::Layer& layer, double seconds,
                    const core::SizeOf& sizes) {
    const core::TimeContext ctx = comp.timeContext();
    const core::Transform2D toComp = core::resolvedTransform(
        comp, layer, seconds, ctx, static_cast<double>(comp.width),
        static_cast<double>(comp.height), sizes);
    // Anchor is the origin of layer space, so the pivot is just where that origin lands.
    return QPointF(toComp.applyX(0.0, 0.0), toComp.applyY(0.0, 0.0));
}

core::Value valueOf(const core::Layer& layer, const char* key, double seconds,
                    const core::TimeContext& ctx, core::Value fallback) {
    const core::Property* p = layer.find(key);
    return p != nullptr ? core::evaluate(layer, *p, seconds, ctx) : fallback;
}

// Writes like the inspector does: new keyframe at playhead if animated, else the
// static value. Otherwise the drag fights the existing animation.
void writeValue(core::Property& prop, const core::Value& v, double seconds,
                const core::TimeContext& ctx) {
    if (prop.animated()) {
        core::Keyframe k;
        k.time = core::TimeValue::seconds(seconds);
        k.value = v;
        k.interp = core::Interpolation::Bezier;
        k.easeIn = prop.keys.front().easeIn;
        k.easeOut = prop.keys.front().easeOut;
        prop.addKey(k, ctx);
    } else {
        prop.staticValue = v;
    }
}

}  // namespace

void GpuViewport::mousePressEvent(QMouseEvent* e) {
    if (comp_ == nullptr || e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    // Converted once here; everything below works in surface pixels (what was drawn).
    const QPointF pos = toSurface(e->position());

    // Hand/Zoom/Shape/Pen/Text: no-op for now, but swallow the click rather than fall
    // through to selection.
    if (tool_ != ToolIcon::Selection && tool_ != ToolIcon::Rotation &&
        tool_ != ToolIcon::Anchor) {
        return;
    }

    // A handle on the already-selected layer takes priority over re-picking underneath it.
    if (tool_ == ToolIcon::Selection && selected_.has_value()) {
        if (const core::Layer* layer = comp_->find(*selected_);
            layer != nullptr && layer->kind != core::LayerKind::Audio) {
            const core::Transform2D toWidget =
                unitToWidget(*comp_, *layer, currentTime_, layerSizes_, surfaceFit());
            if (const std::optional<HandleSpec> handle = hitHandle(toWidget, pos)) {
                const core::TimeContext ctx = comp_->timeContext();
                grab_ = Grab::Scale;
                grabStart_ = pos;
                grabOriginal_ = valueOf(*layer, "scale", currentTime_, ctx,
                                        core::Value::vec2(100.0, 100.0));
                grabHandleU_ = handle->u;
                grabHandleV_ = handle->v;
                grabAffectsX_ = handle->affectsX;
                grabAffectsY_ = handle->affectsY;
                grabBack_ = toWidget.inverse();
                emit manipulationBegan(QStringLiteral("Scale Layer"));
                return;
            }
        }
    }

    const std::optional<core::LayerId> hit = layerAt(pos);
    if (!hit.has_value()) {
        return;  // clicking the letterbox is not a deselect: too easy to do by accident
    }
    if (selected_ != hit) {
        selected_ = hit;
        emit layerPicked(*hit);
    }

    const core::Layer* layer = comp_->find(*hit);
    if (layer == nullptr) {
        return;
    }
    const core::TimeContext ctx = comp_->timeContext();
    grabStart_ = pos;

    switch (tool_) {
        case ToolIcon::Rotation: {
            grab_ = Grab::Rotate;
            grabOriginal_ = valueOf(*layer, "rotation", currentTime_, ctx,
                                    core::Value::scalar(0.0));
            const engine::FrameFit fit = surfaceFit();
            const QPointF pivot = pivotInComp(*comp_, *layer, currentTime_, layerSizes_);
            const double px = fit.toTargetX(pivot.x());
            const double py = fit.toTargetY(pivot.y());
            grabAngle_ = std::atan2(pos.y() - py, pos.x() - px) * 180.0 / M_PI;
            break;
        }
        case ToolIcon::Anchor:
            grab_ = Grab::Anchor;
            grabOriginal_ = valueOf(*layer, "anchor_point", currentTime_, ctx,
                                    core::Value::vec2(0.0, 0.0));
            break;
        default:
            grab_ = Grab::Move;
            grabOriginal_ = valueOf(*layer, "position", currentTime_, ctx,
                                    core::Value::vec2(50.0, 50.0));
            break;
    }
    emit manipulationBegan(grab_ == Grab::Rotate   ? QStringLiteral("Rotate Layer")
                           : grab_ == Grab::Anchor ? QStringLiteral("Move Anchor Point")
                                                   : QStringLiteral("Move Layer"));
}

void GpuViewport::mouseMoveEvent(QMouseEvent* e) {
    if (grab_ == Grab::None || comp_ == nullptr || !selected_.has_value()) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    core::Layer* layer = const_cast<core::Layer*>(comp_->find(*selected_));
    if (layer == nullptr) {
        return;
    }
    const core::TimeContext ctx = comp_->timeContext();
    const engine::FrameFit fit = surfaceFit();
    const QPointF pos = toSurface(e->position());

    switch (grab_) {
        case Grab::Move: {
            core::Property* p = layer->find("position");
            if (p == nullptr || fit.scale <= 0.0) {
                return;
            }
            // Measured in composition pixels (not widget) so it tracks the cursor at any
            // zoom, then converted into parent space since Position is stored there.
            double dx = (pos.x() - grabStart_.x()) / fit.scale;
            double dy = (pos.y() - grabStart_.y()) / fit.scale;
            if (layer->parent.has_value()) {
                if (const core::Layer* owner = comp_->find(*layer->parent);
                    owner != nullptr) {
                    const core::Transform2D back =
                        core::resolvedTransform(*comp_, *owner, currentTime_, ctx,
                                                static_cast<double>(comp_->width),
                                                static_cast<double>(comp_->height),
                                                layerSizes_)
                            .inverse();
                    const double ux = back.applyX(dx, dy) - back.applyX(0.0, 0.0);
                    const double uy = back.applyY(dx, dy) - back.applyY(0.0, 0.0);
                    dx = ux;
                    dy = uy;
                }
            }
            // Shift constrains to one axis, decided once rather than every move.
            if (e->modifiers().testFlag(Qt::ShiftModifier)) {
                if (std::fabs(pos.x() - grabStart_.x()) >
                    std::fabs(pos.y() - grabStart_.y())) {
                    dy = 0.0;
                } else {
                    dx = 0.0;
                }
            }
            writeValue(*p,
                       core::Value::vec2(
                           grabOriginal_.c[0] + dx / static_cast<double>(comp_->width) * 100.0,
                           grabOriginal_.c[1] + dy / static_cast<double>(comp_->height) * 100.0),
                       currentTime_, ctx);
            break;
        }
        case Grab::Rotate: {
            core::Property* p = layer->find("rotation");
            if (p == nullptr) {
                return;
            }
            const QPointF pivot = pivotInComp(*comp_, *layer, currentTime_, layerSizes_);
            const double px = fit.toTargetX(pivot.x());
            const double py = fit.toTargetY(pivot.y());
            double deg = std::atan2(pos.y() - py, pos.x() - px) * 180.0 / M_PI - grabAngle_;
            // Shift snaps to 15 degree increments.
            double value = grabOriginal_.c[0] + deg;
            if (e->modifiers().testFlag(Qt::ShiftModifier)) {
                value = std::round(value / 15.0) * 15.0;
            }
            writeValue(*p, core::Value::scalar(value), currentTime_, ctx);
            break;
        }
        case Grab::Anchor: {
            core::Property* p = layer->find("anchor_point");
            if (p == nullptr || fit.scale <= 0.0 || !layerSizes_) {
                return;
            }
            // Anchor is a % of the layer, drag arrives in screen pixels, so invert through
            // the layer's own transform — composition pixels would be wrong once
            // scaled/rotated.
            const core::Transform2D back =
                unitToWidget(*comp_, *layer, currentTime_, layerSizes_, fit).inverse();
            const double u0 = back.applyX(grabStart_.x(), grabStart_.y());
            const double v0 = back.applyY(grabStart_.x(), grabStart_.y());
            const double u1 = back.applyX(pos.x(), pos.y());
            const double v1 = back.applyY(pos.x(), pos.y());
            writeValue(*p,
                       core::Value::vec2(grabOriginal_.c[0] + (u1 - u0) * 100.0,
                                         grabOriginal_.c[1] + (v1 - v0) * 100.0),
                       currentTime_, ctx);
            break;
        }
        case Grab::Scale: {
            core::Property* p = layer->find("scale");
            if (p == nullptr) {
                return;
            }
            // Uses the box-to-widget transform as it stood at press, held fixed for the
            // drag (same as Rotate's fixed pivot angle), so the math doesn't chase its
            // own output.
            const double u1 = grabBack_.applyX(pos.x(), pos.y());
            const double v1 = grabBack_.applyY(pos.x(), pos.y());

            const core::Value anchor = valueOf(*layer, "anchor_point", currentTime_, ctx,
                                               core::Value::vec2(0.0, 0.0));
            const double au = 0.5 + anchor.c[0] / 100.0;
            const double av = 0.5 + anchor.c[1] / 100.0;

            // Pivots at the anchor point; new scale is whatever ratio keeps the grabbed
            // handle under the cursor.
            double sx = grabOriginal_.c[0];
            double sy = grabOriginal_.c[1];
            constexpr double kMinDenom = 1e-3;
            if (grabAffectsX_ && std::fabs(grabHandleU_ - au) > kMinDenom) {
                sx = grabOriginal_.c[0] * (u1 - au) / (grabHandleU_ - au);
            }
            if (grabAffectsY_ && std::fabs(grabHandleV_ - av) > kMinDenom) {
                sy = grabOriginal_.c[1] * (v1 - av) / (grabHandleV_ - av);
            }
            // Shift on a corner handle keeps aspect ratio, driven by the larger-delta axis.
            if (grabAffectsX_ && grabAffectsY_ &&
                e->modifiers().testFlag(Qt::ShiftModifier)) {
                const double rx = grabOriginal_.c[0] != 0.0 ? sx / grabOriginal_.c[0] : 1.0;
                const double ry = grabOriginal_.c[1] != 0.0 ? sy / grabOriginal_.c[1] : 1.0;
                const double r = std::fabs(rx - 1.0) >= std::fabs(ry - 1.0) ? rx : ry;
                sx = grabOriginal_.c[0] * r;
                sy = grabOriginal_.c[1] * r;
            }
            writeValue(*p, core::Value::vec2(sx, sy), currentTime_, ctx);
            break;
        }
        case Grab::None:
            return;
    }
    emit layerTransformed();
    update();
}

void GpuViewport::mouseReleaseEvent(QMouseEvent* e) {
    if (grab_ == Grab::None) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    grab_ = Grab::None;
    emit manipulationEnded();
}

std::vector<std::pair<double, bool>> GpuViewport::cachedFrames(double from,
                                                               double to) const {
    std::vector<std::pair<double, bool>> out;
    if (compositor_ == nullptr || comp_ == nullptr || project_ == nullptr) {
        return out;
    }
    const double fps = comp_->fps > 0.0 ? comp_->fps : 30.0;
    const engine::FrameCache& cache = compositor_->cache();

    engine::ExternalKeys keys;
    for (const auto& [id, text] : textTextures_) {
        keys.emplace(id, static_cast<std::uint64_t>(text.key));
    }

    // Capped: a long comp at 60fps is hundreds of thousands of frames, far more than a
    // pixel-strip UI needs to draw.
    const auto first = static_cast<std::int64_t>(std::floor(from * fps));
    const auto last = static_cast<std::int64_t>(std::ceil(to * fps));
    constexpr std::int64_t kMaxFrames = 4000;
    const std::int64_t stride = std::max<std::int64_t>(1, (last - first) / kMaxFrames);

    for (std::int64_t f = first; f <= last; f += stride) {
        const double t = static_cast<double>(f) / fps;
        const engine::RenderGraph graph =
            engine::Compositor::graphFor(*project_, *comp_, t, &keys);

        // Ready when every layer needing a cached texture has one; a layer with no
        // effects needs nothing since its source is its own output.
        bool ready = graph.root >= 0;
        for (const int node : graph.nodes[static_cast<std::size_t>(graph.root)].inputs) {
            const engine::RenderNode& n = graph.nodes[static_cast<std::size_t>(node)];
            if (n.kind == engine::RenderNode::Kind::Effect && !cache.contains(n.hash)) {
                ready = false;
                break;
            }
        }
        if (ready) {
            out.emplace_back(t, false);  // RAM. There is no disk tier yet.
        }
    }
    return out;
}

}  // namespace ruby::ui
