#include "ruby/engine/Compositor.h"

#include "ruby/core/Expressions.h"
#include "ruby/core/Transform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ruby::engine {
namespace {

// Engine-internal pass, so WGSL rather than Slang (see GpuDevice.h). Vertices are
// generated from the vertex index; there is nothing to bind but the uniforms.
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
    // Layers without media sample a 1x1 white texture, so one pipeline covers both
    // cases instead of two that have to be kept in step.
    let c = textureSample(tex, samp, in.uv) * u.color;

    // PREMULTIPLIED output. Every blend mode's factors are written assuming this, and
    // for Normal it lands on exactly the same result as the straight-alpha blending this
    // replaced. Without it, Add and Screen blow out wherever a layer is semi-transparent,
    // because the hardware would add the full colour regardless of coverage.
    return vec4<f32>(c.rgb * c.a, c.a);
}
)";

// The uniform block is 80 bytes but WebGPU wants uniform bindings aligned; 256 is the
// conservative floor across backends.
constexpr std::size_t kUniformStride = 256;

struct Rgb {
    float r;
    float g;
    float b;
};

// Mirrors the WGSL EffectUniforms block: eight vec4 slots, filled in schema order.
struct EffectUniforms {
    float params[8][4];
};

// Layer label colours, converted from the UI's sRGB hexes to linear light. Compositing
// happens in linear, and the surface is sRGB, so the hardware re-encodes on write.
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
        return {};  // unopenable file; the layer stays flat rather than vanishing
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
        // sRGB so sampling converts to linear for us; the working space is linear light.
        desc.format = gpu::TextureFormat::RGBA8UnormSrgb;
        desc.usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst;
        desc.debug_label = "video frame";
        source.texture = device_.create_texture(desc);
        source.uploadedTime = -1.0;
    }

    // Re-uploading an unchanged frame is 8MB of pointless traffic per repaint, and the
    // viewer repaints for reasons that have nothing to do with time.
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

    // The four that need to read the destination cannot be a blend equation at all, so
    // they fall back to Normal rather than silently rendering as something they are not.
    // Better a layer that looks unblended than one that looks blended wrongly.
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
    // Effects render into the linear working format, not the display format, so their
    // output stays in linear light for the next effect in the chain.
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
    // RGBA16Float, not 8-bit: effects stack, and 8 bits per channel bands visibly after
    // two or three passes. Headroom above 1.0 also matters for glows later.
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
        // A layer with no effects has nothing worth caching: its output IS its source, the
        // decoder already holds that texture, and storing a second reference to it would
        // spend the budget on the one thing that was already free.
        return source;
    }

    // Everything that will actually draw, decided before anything does, so the last pass
    // is known and can be sent somewhere the cache can keep.
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

    // The cheapest render is the one that never starts. Asked before a texture is touched
    // and before a single pass is recorded.
    if (outputHash != 0) {
        if (gpu::TextureHandle hit = cache_.find(outputHash); hit != nullptr) {
            return hit;
        }
    }

    const Workspace& ws = workspaceFor(layer.id, source->width(), source->height());
    if (ws.a == nullptr || ws.b == nullptr) {
        return source;
    }

    // The last pass draws into a texture of its own rather than into the ping-pong pair,
    // because the pair is reused by this layer on the very next frame. A cached handle
    // pointing at a workspace would be overwritten and the cache would start serving the
    // wrong picture, confidently.
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

        // Parameters go into the uniform block in schema order, one vec4 each, so the
        // shader indexes them positionally and nothing here is effect-specific.
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
        // RGBA16Float: eight bytes a pixel. Counted honestly, because a budget measured in
        // entries would let eight 1080x1920 textures quietly become a gigabyte.
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

    // Fit the composition inside the target, preserving its aspect. Through the shared
    // helper, because the viewport has to turn a mouse position into a composition
    // position and a click two pixels from where the picture was drawn is a click on the
    // wrong layer.
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

    // What this frame is made of, worked out before any of it is drawn. Pure, cheap, and
    // the only thing that can answer "have we already got this".
    const RenderGraph graph = buildGraph(project, comp, seconds, externalKeys);

    // Everything below renders at the FRAME's time, not at wherever the playhead happens
    // to sit inside it.
    //
    // The graph quantises time so that two scrubs landing on the same frame produce the
    // same hash. If the render did not quantise with it, a cache hit would hand back a
    // texture drawn at 1.0033s while claiming to be frame 30, and a scrub across one frame
    // would show whichever sub-frame position happened to render first. A cache that is
    // confidently wrong is worse than no cache, and this is the line that decides which
    // one this is.
    const double frameFps = comp.fps > 0.0 ? comp.fps : 30.0;
    seconds = static_cast<double>(graph.frame) / frameFps;

    auto commands = device_.begin_commands("composite");

    // Effect passes run first, into their own targets. Render passes cannot nest, so a
    // layer's stack has to be finished before the pass that draws the frame opens.
    std::size_t slot = 0;
    struct Prepared {
        const core::Layer* layer;
        gpu::TextureHandle texture;
        Content content;
        double in;
    };
    std::vector<Prepared> prepared;
    prepared.reserve(comp.layers.size());

    // Solo is a whole-composition question, so it has to be answered before any single
    // layer can be judged: one soloed layer changes what every other layer does. Asking
    // per layer would need this same scan each time.
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

        // With anything soloed, only soloed layers are candidates. The eye still applies
        // to those, below: solo narrows the set, visibility decides within it. Two
        // independent switches, which is easier to predict than one overriding the other.
        if (anySolo && !layer.solo) {
            continue;
        }
        // Nulls are never drawn. A null exists to be parented to: it is a transform with
        // a handle, and rendering it would put a coloured rectangle in the middle of
        // every shot that used one.
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
            // Supplied content, currently only text. Indistinguishable from footage from
            // here on: it is a texture with a size, and every sizing, effect and blend
            // path treats it the same way.
            if (const auto supplied = external->find(layer.id);
                supplied != external->end()) {
                content.texture = supplied->second.texture;
                content.width = supplied->second.width;
                content.height = supplied->second.height;
            }
        }
        // The hash of this layer's finished output, from the graph. Zero when the graph
        // does not have a node for it, which means "render it and do not cache it" rather
        // than a guessed key.
        NodeHash outputHash = 0;
        if (const int node = graph.outputFor(layer.id); node >= 0) {
            outputHash = graph.nodes[static_cast<std::size_t>(node)].hash;
        }
        gpu::TextureHandle texture = applyEffects(*commands, layer, content.texture,
                                                  seconds, ctx, slot, outputHash);
        prepared.push_back({&layer, std::move(texture), content, in});
    }

    // Outside the frame is near-black so the letterbox reads as "not your picture".
    commands->begin_pass(target, 0.008f, 0.008f, 0.008f, 1.0f);

    // Everything from here on is clipped to the composition frame. A layer that animates
    // off the edge has to actually leave the picture; without this it keeps drawing over
    // the letterbox and the frame boundary means nothing. The clear above is unaffected,
    // since it applies to the whole attachment rather than the scissor.
    const auto clampToTarget = [](float value, float limit) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0f, limit));
    };
    commands->set_scissor(clampToTarget(frameX, viewW), clampToTarget(frameY, viewH),
                          clampToTarget(frameW, viewW - frameX),
                          clampToTarget(frameH, viewH - frameY));

    // Draw the frame itself, so an empty composition still shows where it is.
    // Takes a transform from the unit quad straight to screen pixels, rather than a
    // rectangle. A rectangle cannot express rotation, which is why Rotation and Anchor
    // Point sat in the inspector doing nothing until now.
    const auto pushQuad = [&](const core::Transform2D& unitToScreen, Rgb color, float alpha,
                              const gpu::TextureHandle& texture,
                              const gpu::RenderPipelineHandle& pipeline) {
        // Screen pixels -> normalised device coordinates, folded into the same matrix.
        // Y flips because NDC is up-positive and our layout is top-down.
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

    // How big a layer is in COMPOSITION units, before scale.
    //
    // Everything here used to be computed in screen pixels, which quietly baked the frame
    // fit into the layer's size. Keeping it in composition units means the transform can
    // do the placing and the screen conversion happens exactly once, at the end.
    const auto sizeOf = [&](const core::Layer& layer,
                            const Content& content) -> core::LayerSize {
        if (content.width > 0 && content.height > 0) {
            // A layer is the size of its source, not the size of the frame. A 1920x1080
            // clip in a 1080x1920 composition comes in wider than the frame and gets
            // cropped at the sides; squashing it to fit would distort the picture and make
            // every framing decision on top of it wrong.
            return {static_cast<double>(content.width),
                    static_cast<double>(content.height)};
        }
        switch (layer.kind) {
            case core::LayerKind::Footage:
            case core::LayerKind::Precomp:
                // No source resolved. A precomp is comp-sized by definition, and
                // unresolved footage has no better guess available.
                return {static_cast<double>(compW), static_cast<double>(compH)};
            case core::LayerKind::Solid:
                // Zero means "match the composition", so a solid follows a comp that gets
                // resized rather than staying frozen at whatever it was created at.
                return {layer.solidWidth > 0 ? static_cast<double>(layer.solidWidth)
                                             : static_cast<double>(compW),
                        layer.solidHeight > 0 ? static_cast<double>(layer.solidHeight)
                                              : static_cast<double>(compH)};
            default:
                return {static_cast<double>(compW) * 0.6, static_cast<double>(compH) * 0.6};
        }
    };

    // The same thing as a plain SizeOf, for parents up the chain. A parent has no Content
    // here: it may not even be drawn this frame, and a null never is. Its size still
    // matters, because its anchor is measured against it.
    const core::SizeOf sizeOf2 = [&](const core::Layer& layer) {
        const Content none;
        return sizeOf(layer, none);
    };

    // A plain rectangle expressed as a transform: scale the unit quad to the frame, then
    // move it there.
    const auto rectToScreen = [](double x, double y, double w, double h) {
        return core::Transform2D::scale(w, h).then(core::Transform2D::translate(x, y));
    };
    pushQuad(rectToScreen(static_cast<double>(frameX), static_cast<double>(frameY),
                          static_cast<double>(frameW), static_cast<double>(frameH)),
             linearFrom8Bit(0x14, 0x14, 0x14), 1.0f, nullptr, quads_);

    for (const Prepared& item : prepared) {
        const core::Layer& layer = *item.layer;
        const Content& content = item.content;

        // Through core::evaluate, like every other transform property. Reading it with
        // Property::evaluate meant an expression on Opacity was silently ignored while the
        // identical expression on Position worked, which is the worst kind of
        // inconsistency: it looks like the expression is wrong.
        const core::Property* opacity = layer.find("opacity");
        const double alphaPct =
            opacity != nullptr ? core::evaluate(layer, *opacity, seconds, ctx).c[0] : 100.0;
        const auto alpha = static_cast<float>(std::isfinite(alphaPct) ? alphaPct : 100.0);

        // Media fills the quad; without it the label colour stands in.
        Rgb tint = (content.texture != nullptr) ? Rgb{1.0f, 1.0f, 1.0f}
                                               : colorFor(layer.label);

        // A solid is its own colour, not its label colour. The label is organisational;
        // the colour is the picture.
        if (layer.kind == core::LayerKind::Solid) {
            tint = Rgb{static_cast<float>(layer.solidColor.c[0]),
                       static_cast<float>(layer.solidColor.c[1]),
                       static_cast<float>(layer.solidColor.c[2])};
        }

        const core::LayerSize size = sizeOf(layer, content);

        // Unit quad -> the layer's own space, centred. Centred because the anchor point is
        // measured from the middle of the layer, so an untouched layer pivots about
        // itself.
        const core::Transform2D unitToLayer =
            core::Transform2D::translate(-0.5, -0.5)
                .then(core::Transform2D::scale(size.width, size.height));

        // Layer space -> composition space, including scale, rotation, anchor and every
        // parent up the chain.
        const core::Transform2D layerToComp =
            core::resolvedTransform(comp, layer, seconds, ctx, static_cast<double>(compW),
                                    static_cast<double>(compH), sizeOf2);

        // Composition space -> screen pixels. One uniform factor, because the frame fit is
        // uniform: a composition pixel is the same size horizontally and vertically.
        const float pxPerUnit = frameW / compW;
        const core::Transform2D compToScreen =
            core::Transform2D::scale(static_cast<double>(pxPerUnit),
                                     static_cast<double>(pxPerUnit))
                .then(core::Transform2D::translate(static_cast<double>(frameX),
                                                   static_cast<double>(frameY)));

        // item.texture is the effect stack's output, or the raw source when the layer
        // has no effects.
        pushQuad(unitToLayer.then(layerToComp).then(compToScreen), tint,
                 std::clamp(alpha / 100.0f, 0.0f, 1.0f), item.texture,
                 quadPipelineFor(layer.blend));
    }

    // Handles and guides, last and on top of everything.
    //
    // The scissor is deliberately widened to the whole target first. A layer dragged half
    // out of frame still has handles, and clipping them to the frame would hide the corner
    // you are reaching for exactly when you most need it.
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
