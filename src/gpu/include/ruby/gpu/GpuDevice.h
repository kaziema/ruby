#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// --- GPU abstraction ---------------------------------------------------------
//
// Backend is Dawn (native WebGPU), but nothing above this header knows that — a 2D
// compositor's GPU surface is small (textures, passes, compute, blits, uploads), so
// swapping backends should cost an afternoon, not a rewrite.
//
// STATUS: device/textures/buffers/submission implemented. Compute pipelines wait on
// the Slang shader toolchain.

namespace ruby::gpu {

enum class TextureFormat {
    RGBA8Unorm,
    RGBA16Float,   // working format for linear-light compositing
    RGBA32Float,
    R8Unorm,       // masks
    R16Float,
    // Display formats — sRGB variants let hardware apply the linear-to-display
    // transform on write, instead of every shader encoding it by hand.
    BGRA8Unorm,
    BGRA8UnormSrgb,
    RGBA8UnormSrgb,
};

enum class TextureUsage : std::uint32_t {
    None       = 0,
    Sampled    = 1u << 0,
    Storage    = 1u << 1,
    RenderTo   = 1u << 2,
    CopySrc    = 1u << 3,
    CopyDst    = 1u << 4,
};

constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(a) |
                                     static_cast<std::uint32_t>(b));
}
constexpr bool has(TextureUsage set, TextureUsage bit) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) != 0u;
}

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::RGBA16Float;
    TextureUsage usage = TextureUsage::Sampled | TextureUsage::RenderTo;
    std::string_view debug_label;
};

class CommandRecorder;

// Opaque resource handles. Backends subclass these; nothing above this header knows
// what a WGPUTexture is.
class Texture {
public:
    virtual ~Texture() = default;
    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    [[nodiscard]] virtual TextureFormat format() const noexcept = 0;
};

class Buffer {
public:
    virtual ~Buffer() = default;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

class ComputePipeline {
public:
    virtual ~ComputePipeline() = default;
};

// A draw pipeline. Engine passes (composite, blit) are a handful of these; effects
// are compute.
// How a draw combines with what's already in the target. Closed enum: these are the
// blend modes expressible as fixed-function GPU state. Overlay/Soft Light/Hard Light/
// Difference need the shader to read the destination — a different mechanism, not
// built here.
//
// Assumes PREMULTIPLIED source color, required for these factors to compose correctly;
// straight alpha would blow out Screen/Add on partly transparent layers.
enum class BlendPreset {
    AlphaOver,  // Normal
    Add,
    Screen,
    Multiply,
    Lighten,
    Darken,
};

class RenderPipeline {
public:
    virtual ~RenderPipeline() = default;
};

using TextureHandle = std::shared_ptr<Texture>;
using BufferHandle = std::shared_ptr<Buffer>;
using ComputePipelineHandle = std::shared_ptr<ComputePipeline>;
using RenderPipelineHandle = std::shared_ptr<RenderPipeline>;

// A window we can present to. The native handle is the only platform-specific thing
// in the whole interface: an NSView* on macOS, an HWND on Windows.
class Surface {
public:
    virtual ~Surface() = default;

    virtual void configure(std::uint32_t width, std::uint32_t height) = 0;

    // What the swapchain actually gave us. Pipelines have to be built against it.
    [[nodiscard]] virtual TextureFormat format() const noexcept = 0;

    // Next backbuffer, or null if unavailable (resizing, occluded, device lost) —
    // callers must handle null.
    [[nodiscard]] virtual TextureHandle acquire() = 0;

    virtual void present() = 0;
};

using SurfaceHandle = std::shared_ptr<Surface>;

class GpuDevice {
public:
    virtual ~GpuDevice() = default;

    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;

    // Resources
    [[nodiscard]] virtual TextureHandle create_texture(const TextureDesc& desc) = 0;
    [[nodiscard]] virtual BufferHandle create_uniform_buffer(std::size_t bytes,
                                                            std::string_view label) = 0;
    virtual void write_buffer(const BufferHandle& dst, const void* data, std::size_t bytes) = 0;
    virtual void write_texture(const TextureHandle& dst, const void* data,
                               std::size_t bytes, std::uint32_t row_stride) = 0;

    // Effects are authored in Slang and compiled to the backend's target.
    [[nodiscard]] virtual ComputePipelineHandle create_compute_pipeline(
        std::string_view slang_module, std::string_view entry_point) = 0;

    // Engine-internal draw passes, written in WGSL directly — a fixed handful we
    // write once. Slang's module system pays off on the effect library, not here.
    [[nodiscard]] virtual RenderPipelineHandle create_render_pipeline(
        std::string_view wgsl, std::string_view vertex_entry,
        std::string_view fragment_entry, TextureFormat target_format,
        std::string_view label, BlendPreset blend = BlendPreset::AlphaOver) = 0;

    // Presentation. `native_window` is an NSView* on macOS, an HWND on Windows.
    [[nodiscard]] virtual SurfaceHandle create_surface(void* native_window) = 0;

    // Work submission
    [[nodiscard]] virtual std::unique_ptr<CommandRecorder> begin_commands(
        std::string_view label) = 0;
    virtual void submit(std::unique_ptr<CommandRecorder> recorder) = 0;

    // Blocks until all submitted work completes. Export path only; never the UI thread.
    virtual void wait_idle() = 0;

    // Human-readable adapter description, for diagnostics and the about box.
    [[nodiscard]] virtual std::string description() const = 0;

protected:
    GpuDevice() = default;
};

// Records a batch of GPU work. One recorder per render of one frame region.
class CommandRecorder {
public:
    virtual ~CommandRecorder() = default;

    virtual void dispatch(const ComputePipelineHandle& pipeline,
                          std::uint32_t groups_x,
                          std::uint32_t groups_y) = 0;

    virtual void bind_texture(std::uint32_t slot, const TextureHandle& tex) = 0;
    virtual void bind_storage_texture(std::uint32_t slot, const TextureHandle& tex) = 0;
    virtual void bind_uniforms(std::uint32_t slot, const BufferHandle& buf) = 0;

    virtual void copy_texture(const TextureHandle& src, const TextureHandle& dst) = 0;

    // Opens a render pass, clearing the target. Color components are 0..1 in linear
    // light (the working space everything composites in).
    virtual void begin_pass(const TextureHandle& target, float r, float g, float b,
                            float a) = 0;
    virtual void end_pass() = 0;

    // Restricts drawing to a pixel rect; the clear still applies to the whole attachment.
    virtual void set_scissor(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                             std::uint32_t height) = 0;

    // Draws `vertex_count` vertices; generated in the shader, so no vertex buffer to
    // bind. `texture` is sampled by the pipeline — flat color is a 1x1 white texture,
    // not a second pipeline.
    virtual void draw(const RenderPipelineHandle& pipeline, const BufferHandle& uniforms,
                      const TextureHandle& texture, std::uint32_t vertex_count) = 0;
};

// Backend factory. Returns nullptr if no suitable adapter exists.
[[nodiscard]] std::unique_ptr<GpuDevice> create_dawn_device();

}  // namespace ruby::gpu
