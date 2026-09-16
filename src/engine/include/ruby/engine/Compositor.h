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

// Draws a composition to a render target, fitted and letterboxed to the target's aspect.
//
// Layers render as flat-color quads; media/text/solids just change what fills the quad.
// Transform math, ordering, and blending are the same either way.
class Compositor {
public:
    Compositor(gpu::GpuDevice& device, gpu::TextureFormat targetFormat);

    // Content a layer has that the engine can't produce itself (currently text, which
    // needs Qt's font stack in the UI module; caller hands over a ready texture).
    struct External {
        gpu::TextureHandle texture;
        int width = 0;
        int height = 0;
    };
    using ExternalTextures = std::map<core::LayerId, External>;

    // Flat shapes (selection handles, etc.) drawn on top, in target pixels. A transform
    // from the unit quad, not a rectangle, so handles on a rotated layer can rotate too.
    // The engine never interprets these; it just draws what it's given.
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

    // Where the frame lands in a target of this size; shared with the renderer so hit
    // testing and drawing agree.
    [[nodiscard]] static FrameFit fitFor(const core::Composition& comp, double targetWidth,
                                         double targetHeight) noexcept {
        return frameFit(static_cast<double>(comp.width), static_cast<double>(comp.height),
                        targetWidth, targetHeight);
    }

    // RAM tier of the preview cache. Exposed for the ready-state UI and for resolution
    // changes to drop it wholesale.
    [[nodiscard]] FrameCache& cache() noexcept { return cache_; }
    [[nodiscard]] const FrameCache& cache() const noexcept { return cache_; }

    // Builds the graph for one instant without rendering it, e.g. for the cache bar to
    // check readiness without drawing.
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

    // Opened file plus its current frame's texture. Kept per path so shared clips share
    // one decoder.
    struct Source {
        std::unique_ptr<media::VideoDecoder> decoder;
        gpu::TextureHandle texture;
        double uploadedTime = -1.0;
    };

    // Ping-pong textures a layer's effect stack runs through, at source resolution in
    // the linear working format.
    struct Workspace {
        gpu::TextureHandle a;
        gpu::TextureHandle b;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // Recycled across frames rather than reallocated per layer.
    [[nodiscard]] gpu::BufferHandle uniformBuffer(std::size_t index);

    // Lazily compiled, one per effect id, then cached.
    [[nodiscard]] gpu::RenderPipelineHandle pipelineFor(const EffectDef& def);

    [[nodiscard]] Workspace& workspaceFor(core::LayerId layer, std::uint32_t width,
                                          std::uint32_t height);

    // Runs a layer's effect stack; returns `source` unchanged if no effects are enabled.
    [[nodiscard]] gpu::TextureHandle applyEffects(gpu::CommandRecorder& commands,
                                                  const core::Layer& layer,
                                                  const gpu::TextureHandle& source,
                                                  double seconds,
                                                  const core::TimeContext& ctx,
                                                  std::size_t& slot, NodeHash outputHash);

    // Texture the cache can own, separate from the reused ping-pong pair.
    [[nodiscard]] gpu::TextureHandle createOutputTexture(std::uint32_t width,
                                                         std::uint32_t height);

    // A layer's source material: texture plus its wanted size.
    struct Content {
        gpu::TextureHandle texture;
        int width = 0;
        int height = 0;
    };

    // Decodes/uploads the frame for `path` at `seconds`. Empty result (unopenable file)
    // leaves the layer flat rather than making it disappear.
    [[nodiscard]] Content contentFor(const std::string& path, double seconds);

    gpu::GpuDevice& device_;
    // One quad pipeline per blend mode (blend equation is baked into the pipeline).
    [[nodiscard]] gpu::RenderPipelineHandle quadPipelineFor(core::BlendMode mode);

    gpu::RenderPipelineHandle quads_;
    std::map<core::BlendMode, gpu::RenderPipelineHandle> quadPipelines_;
    gpu::TextureFormat targetFormat_ = gpu::TextureFormat::BGRA8UnormSrgb;
    std::vector<gpu::BufferHandle> uniforms_;
    gpu::TextureHandle white_;  // stand-in so layers without media use one pipeline
    std::unordered_map<std::string, Source> sources_;
    std::unordered_map<std::string, gpu::RenderPipelineHandle> effectPipelines_;
    std::unordered_map<core::LayerId, Workspace> workspaces_;

    // 768MB: ~48 1080x1920 layer outputs. Empirical, tune against real usage.
    FrameCache cache_{768u * 1024u * 1024u};
};

}  // namespace ruby::engine
