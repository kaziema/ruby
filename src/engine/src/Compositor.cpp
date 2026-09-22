#include "ruby/engine/Compositor.h"

#include "ruby/core/Expressions.h"
#include "ruby/core/Transform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ruby::engine {
namespace {

// Engine-internal pass: WGSL, not Slang (see GpuDevice.h). Vertices come from the
// vertex index; only uniforms are bound.
constexpr const char* kQuadShader = R"(
struct Uniforms {
    transform : mat4x4<f32>,
    color     : vec4<f32>,
};
@group(0) @binding(0) var<uniform> u   : Uniforms;
@group(0) @binding(1) var        samp : sampler;
@group(0) @binding(2) var        tex  : texture_2d<f32>;

struct VsOut {
    @builtin(position) position : vec4<f32>,
    @location(0)       uv       : vec2<f32>,
};

@vertex
fn vs(@builtin(vertex_index) index : u32) -> VsOut {
    var corners = array<vec2<f32>, 6>(
        vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
        vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));
    var out : VsOut;
    out.position = u.transform * vec4<f32>(corners[index], 0.0, 1.0);
    out.uv = corners[index];
    return out;
}

@fragment
fn fs(in : VsOut) -> @location(0) vec4<f32> {
    // Layers without media sample a 1x1 white texture, so one pipeline covers both cases.
    let c = textureSample(tex, samp, in.uv) * u.color;

    // Premultiplied output: blend modes assume it, and Add/Screen would blow out on
    // semi-transparent layers otherwise.
    return vec4<f32>(c.rgb * c.a, c.a);
}
)";

// Largest block is EffectUniforms (336 bytes); a multiple of 256, the safe alignment
// floor across backends.
constexpr std::size_t kUniformStride = 512;

struct Rgb {
    float r;
    float g;
    float b;
};

// Mirrors the WGSL EffectUniforms block in EffectRegistry.cpp: params in schema order,
// then three vec4 per active mask, then the mask count.
struct EffectUniforms {
    float params[8][4];
    float masks[core::EffectInstance::kMaxMasks * 3][4];
    float maskInfo[4];
};
static_assert(sizeof(EffectUniforms) <= kUniformStride);

void packMasks(const core::EffectInstance& effect, double seconds,
               const core::TimeContext& ctx, EffectUniforms& u) {
    const std::vector<const core::Mask*> masks = core::activeMasks(effect);
    const auto value = [&](const core::Mask& m, const char* key) {
        const core::Property* p = m.find(key);
        return p != nullptr ? p->evaluate(seconds, ctx) : core::Value{};
    };
    for (std::size_t i = 0; i < masks.size(); ++i) {
        const core::Mask& m = *masks[i];
        const core::Value center = value(m, "center");
        const core::Value size = value(m, "size");
        float* m0 = u.masks[i * 3];
        float* m1 = u.masks[i * 3 + 1];
        float* m2 = u.masks[i * 3 + 2];
        m0[0] = static_cast<float>(center.c[0]);
        m0[1] = static_cast<float>(center.c[1]);
        m0[2] = static_cast<float>(size.c[0]);
        m0[3] = static_cast<float>(size.c[1]);
        m1[0] = static_cast<float>(value(m, "rotation").x());
        m1[1] = static_cast<float>(value(m, "feather").x());
        m1[2] = static_cast<float>(value(m, "expansion").x());
        m1[3] = static_cast<float>(value(m, "opacity").x());
        m2[0] = m.shape == core::MaskShape::Ellipse ? 1.0f : 0.0f;
        m2[1] = m.mode == core::MaskMode::Subtract    ? 1.0f
                : m.mode == core::MaskMode::Intersect ? 2.0f
                                                      : 0.0f;
        m2[2] = m.inverted ? 1.0f : 0.0f;
    }
    u.maskInfo[0] = static_cast<float>(masks.size());
}

// UI's sRGB label hexes converted to linear, since compositing happens in linear light.
float toLinear(float srgb) noexcept {
    return srgb <= 0.04045f ? srgb / 12.92f
                            : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

Rgb linearFrom8Bit(int r, int g, int b) noexcept {
    return {toLinear(static_cast<float>(r) / 255.0f),
            toLinear(static_cast<float>(g) / 255.0f),
            toLinear(static_cast<float>(b) / 255.0f)};
}

Rgb colorFor(core::LabelColor label) noexcept {
    switch (label) {
        case core::LabelColor::Lavender: return linearFrom8Bit(0x6e, 0x5b, 0x8a);
        case core::LabelColor::Aqua:     return linearFrom8Bit(0x3f, 0x5f, 0x7d);
        case core::LabelColor::Green:    return linearFrom8Bit(0x4c, 0x6b, 0x40);
        case core::LabelColor::Gray:     break;
    }
    return linearFrom8Bit(0x4a, 0x4a, 0x4a);
}


}  // namespace

Compositor::Compositor(gpu::GpuDevice& device, gpu::TextureFormat targetFormat)
    : device_(device), targetFormat_(targetFormat) {
    quads_ = device_.create_render_pipeline(kQuadShader, "vs", "fs", targetFormat,
                                            "composite quads");
    quadPipelines_.emplace(core::BlendMode::Normal, quads_);

    gpu::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = gpu::TextureFormat::RGBA8Unorm;
    desc.usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst;
    desc.debug_label = "white";
    white_ = device_.create_texture(desc);

    const std::uint8_t pixel[4] = {255, 255, 255, 255};
    device_.write_texture(white_, pixel, sizeof(pixel), 4);
}

Compositor::Content Compositor::contentFor(const std::string& path, double seconds) {
    auto it = sources_.find(path);
    if (it == sources_.end()) {
        Source source;
        source.decoder = media::VideoDecoder::open(path);
        it = sources_.emplace(path, std::move(source)).first;
    }
    Source& source = it->second;
    if (source.decoder == nullptr) {
        return {};  // unopenable file; layer stays flat rather than vanishing
    }

    const Content sized{source.texture, source.decoder->width(),
                        source.decoder->height()};

    const media::VideoFrame* frame = source.decoder->frameAt(seconds);
    if (frame == nullptr || !frame->valid()) {
        return sized;
    }

    if (source.texture == nullptr) {
        gpu::TextureDesc desc;
        desc.width = static_cast<std::uint32_t>(frame->width);
        desc.height = static_cast<std::uint32_t>(frame->height);
        // sRGB format converts to linear on sample, matching the linear working space.
        desc.format = gpu::TextureFormat::RGBA8UnormSrgb;
        desc.usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst;
        desc.debug_label = "video frame";
        source.texture = device_.create_texture(desc);
        source.uploadedTime = -1.0;
    }

    // Skip re-upload when the frame hasn't changed; the viewer repaints for reasons
    // unrelated to time.
    if (source.uploadedTime != frame->pts) {
        device_.write_texture(source.texture, frame->rgba.data(), frame->rgba.size(),
                              static_cast<std::uint32_t>(frame->width) * 4);
        source.uploadedTime = frame->pts;
    }
    return {source.texture, frame->width, frame->height};
}

gpu::BufferHandle Compositor::uniformBuffer(std::size_t index) {
    while (uniforms_.size() <= index) {
        uniforms_.push_back(device_.create_uniform_buffer(kUniformStride, "layer quad"));
    }
    return uniforms_[index];
}

gpu::RenderPipelineHandle Compositor::quadPipelineFor(core::BlendMode mode) {
    if (const auto it = quadPipelines_.find(mode); it != quadPipelines_.end()) {
        return it->second;
    }

    // Modes needing to read the destination aren't expressible as a blend equation;
    // fall back to Normal rather than render them wrong.
    gpu::BlendPreset preset{};
    const char* label = "composite quads";
    switch (mode) {
        case core::BlendMode::Add:      preset = gpu::BlendPreset::Add;      label = "quads add";      break;
        case core::BlendMode::Screen:   preset = gpu::BlendPreset::Screen;   label = "quads screen";   break;
        case core::BlendMode::Multiply: preset = gpu::BlendPreset::Multiply; label = "quads multiply"; break;
        case core::BlendMode::Lighten:  preset = gpu::BlendPreset::Lighten;  label = "quads lighten";  break;
        case core::BlendMode::Darken:   preset = gpu::BlendPreset::Darken;   label = "quads darken";   break;
        case core::BlendMode::Normal:
        case core::BlendMode::Overlay:
        case core::BlendMode::SoftLight:
        case core::BlendMode::HardLight:
        case core::BlendMode::Difference:
            quadPipelines_.emplace(mode, quads_);
            return quads_;
    }

    gpu::RenderPipelineHandle pipeline = device_.create_render_pipeline(
        kQuadShader, "vs", "fs", targetFormat_, label, preset);
    if (pipeline == nullptr) {
        pipeline = quads_;
    }
    quadPipelines_.emplace(mode, pipeline);
    return pipeline;
}

gpu::RenderPipelineHandle Compositor::pipelineFor(const EffectDef& def) {
    const auto it = effectPipelines_.find(def.schema.id);
    if (it != effectPipelines_.end()) {
        return it->second;
    }
    // Renders to the linear working format, not the display format, so it stays linear
    // for the next effect in the chain.
    gpu::RenderPipelineHandle pipeline = device_.create_render_pipeline(
        def.shader, "vs", "fs", gpu::TextureFormat::RGBA16Float, def.schema.id);
    effectPipelines_.emplace(def.schema.id, pipeline);
    return pipeline;
}

Compositor::Workspace& Compositor::workspaceFor(core::LayerId layer, std::uint32_t width,
                                                std::uint32_t height) {
    Workspace& ws = workspaces_[layer];
    if (ws.width == width && ws.height == height && ws.a != nullptr) {
        return ws;
    }

    gpu::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    // RGBA16Float: 8-bit bands visibly after a few stacked passes, and glow needs
    // headroom above 1.0.
    desc.format = gpu::TextureFormat::RGBA16Float;
    desc.usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::RenderTo;
    desc.debug_label = "effect workspace";

    ws.a = device_.create_texture(desc);
    ws.b = device_.create_texture(desc);
    ws.width = width;
    ws.height = height;
    return ws;
}

gpu::TextureHandle Compositor::applyEffects(gpu::CommandRecorder& commands,
                                            const core::Layer& layer,
                                            const gpu::TextureHandle& source,
                                            double seconds, const core::TimeContext& ctx,
                                            std::size_t& slot, NodeHash outputHash) {
    if (source == nullptr || layer.effects.empty()) {
        // Nothing to cache: output IS the source, already held by the decoder.
        return source;
    }

    // Resolve passes up front so the last one is known and can target a keepable texture.
    struct Pass {
        const core::EffectInstance* effect;
        const EffectDef* def;
        gpu::RenderPipelineHandle pipeline;
    };
    std::vector<Pass> passes;
    for (const core::EffectInstance& effect : layer.effects) {
        if (!effect.enabled) {
            continue;
        }
        const EffectDef* def = EffectRegistry::instance().find(effect.effectId);
        if (def == nullptr) {
            continue;  // unknown id; skip rather than drop the layer
        }
        const gpu::RenderPipelineHandle pipeline = pipelineFor(*def);
        if (pipeline == nullptr) {
            continue;
        }
        passes.push_back({&effect, def, pipeline});
    }
    if (passes.empty()) {
        return source;
    }

    // Cache check happens before any texture work starts.
    if (outputHash != 0) {
        if (gpu::TextureHandle hit = cache_.find(outputHash); hit != nullptr) {
            return hit;
        }
    }

    const Workspace& ws = workspaceFor(layer.id, source->width(), source->height());
    if (ws.a == nullptr || ws.b == nullptr) {
        return source;
    }

    // Last pass draws into its own texture, not the ping-pong pair: the pair gets reused
    // next frame, so a cached handle into it would silently go stale.
    gpu::TextureHandle keep = outputHash != 0
                                  ? createOutputTexture(source->width(), source->height())
                                  : nullptr;

    gpu::TextureHandle input = source;
    bool toA = true;
    bool ran = false;

    for (std::size_t pass = 0; pass < passes.size(); ++pass) {
        const core::EffectInstance& effect = *passes[pass].effect;
        const EffectDef* def = passes[pass].def;
        const gpu::RenderPipelineHandle pipeline = passes[pass].pipeline;

        // Params go into the uniform block in schema order; shader indexes positionally.
        EffectUniforms u{};
        for (std::size_t i = 0;
             i < def->schema.params.size() &&
             i < static_cast<std::size_t>(EffectDef::kMaxParams);
             ++i) {
            const core::Property* p = effect.find(def->schema.params[i].key);
            const core::Value v = (p != nullptr)
                                      ? p->evaluate(seconds, ctx)
                                      : core::Value::scalar(
                                            def->schema.params[i].default_value);
            for (int c = 0; c < 4; ++c) {
                u.params[i][c] = static_cast<float>(
                    (c < v.count) ? v.c[static_cast<std::size_t>(c)] : 0.0);
            }
        }
        packMasks(effect, seconds, ctx, u);

        const gpu::BufferHandle buffer = uniformBuffer(slot++);
        device_.write_buffer(buffer, &u, sizeof(u));

        const bool last = (pass + 1 == passes.size());
        const gpu::TextureHandle output =
            (last && keep != nullptr) ? keep : (toA ? ws.a : ws.b);
        commands.begin_pass(output, 0.0f, 0.0f, 0.0f, 0.0f);
        commands.draw(pipeline, buffer, input, 3);  // fullscreen triangle
        commands.end_pass();

        input = output;
        toA = !toA;
        ran = true;
    }

    if (!ran) {
        return source;
    }
    if (keep != nullptr && input == keep) {
        // RGBA16Float: 8 bytes/pixel.
        const std::size_t bytes = static_cast<std::size_t>(source->width()) *
                                  static_cast<std::size_t>(source->height()) * 8;
        cache_.put(outputHash, input, bytes);
    }
    return input;
}

gpu::TextureHandle Compositor::createOutputTexture(std::uint32_t width,
                                                   std::uint32_t height) {
    gpu::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = gpu::TextureFormat::RGBA16Float;
    desc.usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::RenderTo;
    desc.debug_label = "cached layer output";
    return device_.create_texture(desc);
}

void Compositor::render(const core::Project& project, const core::Composition& comp,
                        double seconds, const gpu::TextureHandle& target,
                        const ExternalTextures* external, const ExternalKeys* externalKeys,
                        const Overlay* overlay) {
    if (target == nullptr || quads_ == nullptr) {
        return;
    }

    const auto viewW = static_cast<float>(target->width());
    const auto viewH = static_cast<float>(target->height());
    if (viewW <= 0.0f || viewH <= 0.0f) {
        return;
    }

    // Fit composition into target, preserving aspect; shared helper so hit-testing agrees.
    const auto compW = static_cast<float>(comp.width);
    const auto compH = static_cast<float>(comp.height);
    const FrameFit fit_ =
        frameFit(static_cast<double>(comp.width), static_cast<double>(comp.height),
                 static_cast<double>(viewW), static_cast<double>(viewH));
    const auto frameW = static_cast<float>(fit_.width);
    const auto frameH = static_cast<float>(fit_.height);
    const auto frameX = static_cast<float>(fit_.x);
    const auto frameY = static_cast<float>(fit_.y);

    const core::TimeContext ctx = comp.timeContext();

    const RenderGraph graph = buildGraph(project, comp, seconds, externalKeys);

    // Snap to the graph's quantized frame time: without this, a cache hit could return a
    // texture rendered at a different sub-frame position than the graph's hash claims.
    const double frameFps = comp.fps > 0.0 ? comp.fps : 30.0;
    seconds = static_cast<double>(graph.frame) / frameFps;

    auto commands = device_.begin_commands("composite");

    // Effect passes run first, into their own targets: render passes can't nest.
    std::size_t slot = 0;
    struct Prepared {
        const core::Layer* layer;
        gpu::TextureHandle texture;
        Content content;
        double in;
    };
    std::vector<Prepared> prepared;
    prepared.reserve(comp.layers.size());

    // Solo is whole-composition, so resolve it once up front.
    bool anySolo = false;
    for (const core::Layer& layer : comp.layers) {
        if (layer.solo && layer.kind != core::LayerKind::Audio) {
            anySolo = true;
            break;
        }
    }

    // Bottom layer first, so index 0 (the topmost) is drawn last.
    for (auto it = comp.layers.rbegin(); it != comp.layers.rend(); ++it) {
        const core::Layer& layer = *it;

        // Solo narrows the candidate set; the eye toggle still applies within it.
        if (anySolo && !layer.solo) {
            continue;
        }
        // Nulls exist only to be parented to; never drawn.
        if (!layer.enabled || layer.kind == core::LayerKind::Audio ||
            layer.kind == core::LayerKind::Null) {
            continue;
        }
        const double in = to_seconds(layer.inPoint, ctx);
        const double out = to_seconds(layer.outPoint, ctx);
        if (seconds < in || seconds >= out) {
            continue;  // a layer only exists between its in and out points
        }

        Content content;
        if (const std::string path = project.pathFor(layer); !path.empty()) {
            content = contentFor(path, seconds - in);
        } else if (external != nullptr) {
            // Supplied content (currently text only); treated identically to footage
            // from here on since it's just a texture with a size.
            if (const auto supplied = external->find(layer.id);
                supplied != external->end()) {
                content.texture = supplied->second.texture;
                content.width = supplied->second.width;
                content.height = supplied->second.height;
            }
        }
        // Zero means no graph node for this layer: render but don't cache.
        NodeHash outputHash = 0;
        if (const int node = graph.outputFor(layer.id); node >= 0) {
            outputHash = graph.nodes[static_cast<std::size_t>(node)].hash;
        }
        gpu::TextureHandle texture = applyEffects(*commands, layer, content.texture,
                                                  seconds, ctx, slot, outputHash);
        prepared.push_back({&layer, std::move(texture), content, in});
    }

    // Near-black outside the frame so the letterbox reads as "not your picture".
    commands->begin_pass(target, 0.008f, 0.008f, 0.008f, 1.0f);

    // Clip to the composition frame from here on, so layers animating off-edge actually
    // leave the picture. Clear above is unaffected (applies to the whole attachment).
    const auto clampToTarget = [](float value, float limit) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0f, limit));
    };
    commands->set_scissor(clampToTarget(frameX, viewW), clampToTarget(frameY, viewH),
                          clampToTarget(frameW, viewW - frameX),
                          clampToTarget(frameH, viewH - frameY));

    // Takes a transform from the unit quad to screen pixels directly (not a rectangle),
    // since a rectangle can't express rotation.
    const auto pushQuad = [&](const core::Transform2D& unitToScreen, Rgb color, float alpha,
                              const gpu::TextureHandle& texture,
                              const gpu::RenderPipelineHandle& pipeline) {
        // Screen pixels -> NDC, folded into the same matrix. Y flips: NDC is up-positive,
        // our layout is top-down.
        const double ndcX = 2.0 / static_cast<double>(viewW);
        const double ndcY = -2.0 / static_cast<double>(viewH);

        QuadUniforms u{};
        // Column-major, matching WGSL's mat4x4 layout: column 0 is elements 0..3.
        u.transform[0] = static_cast<float>(unitToScreen.a * ndcX);
        u.transform[1] = static_cast<float>(unitToScreen.b * ndcY);
        u.transform[4] = static_cast<float>(unitToScreen.c * ndcX);
        u.transform[5] = static_cast<float>(unitToScreen.d * ndcY);
        u.transform[10] = 1.0f;
        u.transform[12] = static_cast<float>(unitToScreen.tx * ndcX - 1.0);
        u.transform[13] = static_cast<float>(unitToScreen.ty * ndcY + 1.0);
        u.transform[15] = 1.0f;
        u.color[0] = color.r;
        u.color[1] = color.g;
        u.color[2] = color.b;
        u.color[3] = alpha;

        const gpu::BufferHandle buffer = uniformBuffer(slot++);
        device_.write_buffer(buffer, &u, sizeof(u));
        commands->draw(pipeline, buffer, texture != nullptr ? texture : white_, 6);
    };

    // Layer size in composition units, before scale — kept out of screen pixels so the
    // frame-fit conversion happens exactly once, at the end.
    const auto sizeOf = [&](const core::Layer& layer,
                            const Content& content) -> core::LayerSize {
        if (content.width > 0 && content.height > 0) {
            // Layer is sized to its source, not the frame — squashing to fit would
            // distort the picture.
            return {static_cast<double>(content.width),
                    static_cast<double>(content.height)};
        }
        switch (layer.kind) {
            case core::LayerKind::Footage:
            case core::LayerKind::Precomp:
                // No source resolved: precomp is comp-sized by definition; footage has
                // no better guess.
                return {static_cast<double>(compW), static_cast<double>(compH)};
            case core::LayerKind::Solid:
                // 0 means "match the composition" so a solid follows comp resizes.
                return {layer.solidWidth > 0 ? static_cast<double>(layer.solidWidth)
                                             : static_cast<double>(compW),
                        layer.solidHeight > 0 ? static_cast<double>(layer.solidHeight)
                                              : static_cast<double>(compH)};
            default:
                return {static_cast<double>(compW) * 0.6, static_cast<double>(compH) * 0.6};
        }
    };

    // Same as sizeOf, for parents up the chain (which may have no Content this frame).
    const core::SizeOf sizeOf2 = [&](const core::Layer& layer) {
        const Content none;
        return sizeOf(layer, none);
    };

    // Rectangle as a transform: scale unit quad, then translate.
    const auto rectToScreen = [](double x, double y, double w, double h) {
        return core::Transform2D::scale(w, h).then(core::Transform2D::translate(x, y));
    };
    pushQuad(rectToScreen(static_cast<double>(frameX), static_cast<double>(frameY),
                          static_cast<double>(frameW), static_cast<double>(frameH)),
             linearFrom8Bit(0x14, 0x14, 0x14), 1.0f, nullptr, quads_);

    for (const Prepared& item : prepared) {
        const core::Layer& layer = *item.layer;
        const Content& content = item.content;

        // core::evaluate, not Property::evaluate, so expressions on Opacity work like
        // they do on every other property.
        const core::Property* opacity = layer.find("opacity");
        const double alphaPct =
            opacity != nullptr ? core::evaluate(layer, *opacity, seconds, ctx).c[0] : 100.0;
        const auto alpha = static_cast<float>(std::isfinite(alphaPct) ? alphaPct : 100.0);

        // Media fills the quad; without it, the label color stands in.
        Rgb tint = (content.texture != nullptr) ? Rgb{1.0f, 1.0f, 1.0f}
                                               : colorFor(layer.label);

        // A solid uses its own color, not its label color.
        if (layer.kind == core::LayerKind::Solid) {
            tint = Rgb{static_cast<float>(layer.solidColor.c[0]),
                       static_cast<float>(layer.solidColor.c[1]),
                       static_cast<float>(layer.solidColor.c[2])};
        }

        const core::LayerSize size = sizeOf(layer, content);

        // Unit quad -> layer space, centered on the anchor point.
        const core::Transform2D unitToLayer =
            core::Transform2D::translate(-0.5, -0.5)
                .then(core::Transform2D::scale(size.width, size.height));

        // Layer space -> composition space: scale, rotation, anchor, parent chain.
        const core::Transform2D layerToComp =
            core::resolvedTransform(comp, layer, seconds, ctx, static_cast<double>(compW),
                                    static_cast<double>(compH), sizeOf2);

        // Composition space -> screen pixels; one uniform factor since the frame fit
        // is uniform.
        const float pxPerUnit = frameW / compW;
        const core::Transform2D compToScreen =
            core::Transform2D::scale(static_cast<double>(pxPerUnit),
                                     static_cast<double>(pxPerUnit))
                .then(core::Transform2D::translate(static_cast<double>(frameX),
                                                   static_cast<double>(frameY)));

        pushQuad(unitToLayer.then(layerToComp).then(compToScreen), tint,
                 std::clamp(alpha / 100.0f, 0.0f, 1.0f), item.texture,
                 quadPipelineFor(layer.blend));
    }

    // Handles and guides on top of everything; scissor widened to the whole target so
    // handles on a layer dragged out of frame stay visible.
    if (overlay != nullptr && !overlay->empty()) {
        commands->set_scissor(0, 0, static_cast<std::uint32_t>(viewW),
                              static_cast<std::uint32_t>(viewH));
        for (const OverlayQuad& q : *overlay) {
            pushQuad(q.unitToTarget, Rgb{q.r, q.g, q.b}, q.a, nullptr,
                     quadPipelineFor(core::BlendMode::Normal));
        }
    }

    commands->end_pass();
    device_.submit(std::move(commands));
}

}  // namespace ruby::engine
