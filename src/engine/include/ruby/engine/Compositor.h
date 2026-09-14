#pragma once

#include <cstdint>
#include <vector>

#include "ruby/core/Document.h"
#include "ruby/core/Transform.h"
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "ruby/engine/EffectRegistry.h"
#include "ruby/engine/FrameCache.h"
#include "ruby/engine/RenderGraph.h"
#include "ruby/gpu/GpuDevice.h"
#include "ruby/media/VideoDecoder.h"

namespace ruby::engine {

// Draws a composition to a render target.
//
// The composition has its own aspect ratio, which is almost never the shape of the
// window. So it is fitted and letterboxed, and the area outside the frame is painted a
// different colour: without that you cannot tell where the frame actually ends, and
// every framing decision becomes a guess.
//
// Layers currently render as flat colour quads. Media, text, and solids all just change
// what fills the quad; the transform maths, ordering, and blending are the same either
// way and are what this class is really for.
class Compositor {
public:
    Compositor(gpu::GpuDevice& device, gpu::TextureFormat targetFormat);

    // Takes the project because resolving a layer's source means going through the
    // media pool. The composition alone cannot answer "what file is this layer".
    // Content a layer has that the engine cannot produce itself.
    //
    // Text is the reason this exists. Laying out and rasterising type needs a font stack,
    // and the only one in the tree is Qt's, which lives in the UI module. Rather than drag
    // Qt into the engine or write a second font stack, the caller hands over a ready
    // texture and the compositor treats it exactly like decoded footage.
    struct External {
        gpu::TextureHandle texture;
        int width = 0;
        int height = 0;
    };
    using ExternalTextures = std::map<core::LayerId, External>;

    // Flat shapes drawn on top of the picture, in target pixels.
    //
    // Selection handles and the like. Kept as a transform from the unit quad rather than a
    // rectangle for the same reason layers are: a rectangle cannot express rotation, and
    // handles on a rotated layer that stay upright are handles that lie about the layer.
    //
    // The engine draws what it is given and never learns what a handle is. That keeps the
    // decision about what an editor shows you out of the renderer, where it would be one
    // more thing the render path has an opinion about.
    struct OverlayQuad {
        core::Transform2D unitToTarget;
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    };
    using Overlay = std::vector<OverlayQuad>;

    void render(const core::Project& project, const core::Composition& comp,
                double seconds, const gpu::TextureHandle& target,
                const ExternalTextures* external = nullptr,
                const ExternalKeys* externalKeys = nullptr,
                const Overlay* overlay = nullptr);

    // Where the frame lands in a target of this size. The viewport needs the same answer
    // the renderer used, or hit testing and drawing disagree.
    [[nodiscard]] static FrameFit fitFor(const core::Composition& comp, double targetWidth,
                                         double targetHeight) noexcept {
        return frameFit(static_cast<double>(comp.width), static_cast<double>(comp.height),
                        targetWidth, targetHeight);
    }

    // The preview cache's RAM tier: every layer's finished effect output, keyed on what
    // that output depended on. Exposed so the window can report what is ready and so a
    // resolution change can drop everything at once.
    [[nodiscard]] FrameCache& cache() noexcept { return cache_; }
    [[nodiscard]] const FrameCache& cache() const noexcept { return cache_; }

    // The graph for one instant, without rendering it. For the cache bar, which has to
    // answer "is this frame ready" for a whole second of timeline without drawing any of
    // it.
    [[nodiscard]] static RenderGraph graphFor(const core::Project& project,
                                              const core::Composition& comp, double seconds,
                                              const ExternalKeys* externalKeys = nullptr) {
        return buildGraph(project, comp, seconds, externalKeys);
    }

private:
    struct QuadUniforms {
        float transform[16];
        float color[4];
    };

    // An opened file plus the texture its current frame lives in. Kept per path so two
    // layers using the same clip share one decoder.
    struct Source {
        std::unique_ptr<media::VideoDecoder> decoder;
        gpu::TextureHandle texture;
        double uploadedTime = -1.0;
    };

    // Two working textures a layer ping-pongs between while its effect stack runs.
    // Allocated at the source's own resolution, in the linear working format.
    struct Workspace {
        gpu::TextureHandle a;
        gpu::TextureHandle b;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // Uniform buffers are recycled across frames rather than reallocated per layer.
    [[nodiscard]] gpu::BufferHandle uniformBuffer(std::size_t index);

    // Compiled lazily, then kept. One pipeline per effect id.
    [[nodiscard]] gpu::RenderPipelineHandle pipelineFor(const EffectDef& def);

    [[nodiscard]] Workspace& workspaceFor(core::LayerId layer, std::uint32_t width,
                                          std::uint32_t height);

    // Runs a layer's effect stack and returns the texture to sample. Returns `source`
    // unchanged when the layer has no enabled effects, so the common case costs nothing.
    [[nodiscard]] gpu::TextureHandle applyEffects(gpu::CommandRecorder& commands,
                                                  const core::Layer& layer,
                                                  const gpu::TextureHandle& source,
                                                  double seconds,
                                                  const core::TimeContext& ctx,
                                                  std::size_t& slot, NodeHash outputHash);

    // A texture the cache can own, separate from the ping-pong pair a layer reuses every
    // frame.
    [[nodiscard]] gpu::TextureHandle createOutputTexture(std::uint32_t width,
                                                         std::uint32_t height);

    // A layer's source material: the texture to sample plus the size it wants to be.
    // Size matters as much as the pixels; a 1920x1080 clip is not a 1080x1920 layer.
    struct Content {
        gpu::TextureHandle texture;
        int width = 0;
        int height = 0;
    };

    // Decodes and uploads the frame for `path` at `seconds`. An empty result means the
    // file would not open, which leaves the layer flat rather than making it disappear.
    [[nodiscard]] Content contentFor(const std::string& path, double seconds);

    gpu::GpuDevice& device_;
    // One quad pipeline per blend mode, built on first use. The blend equation is baked
    // into a pipeline, so "which blend mode" is a choice of pipeline rather than state we
    // can set per draw. Every quad is already its own draw call, so this costs nothing.
    [[nodiscard]] gpu::RenderPipelineHandle quadPipelineFor(core::BlendMode mode);

    gpu::RenderPipelineHandle quads_;
    std::map<core::BlendMode, gpu::RenderPipelineHandle> quadPipelines_;
    gpu::TextureFormat targetFormat_ = gpu::TextureFormat::BGRA8UnormSrgb;
    std::vector<gpu::BufferHandle> uniforms_;
    gpu::TextureHandle white_;  // stand-in so layers without media use one pipeline
    std::unordered_map<std::string, Source> sources_;
    std::unordered_map<std::string, gpu::RenderPipelineHandle> effectPipelines_;
    std::unordered_map<core::LayerId, Workspace> workspaces_;

    // 768MB. Enough for roughly forty-eight 1080x1920 layer outputs, which is a couple of
    // seconds of a busy composition. A number to tune against a real machine rather than
    // one anybody derived.
    FrameCache cache_{768u * 1024u * 1024u};
};

}  // namespace ruby::engine
