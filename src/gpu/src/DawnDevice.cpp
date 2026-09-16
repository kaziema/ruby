#include <dawn/webgpu_cpp.h>

#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

#include "NativeSurface.h"
#include "ruby/gpu/GpuDevice.h"

// Dawn backend — the only file that knows WebGPU exists; everything above talks to
// GpuDevice.

namespace ruby::gpu {
namespace {

wgpu::TextureFormat toWgpu(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::RGBA8Unorm:     return wgpu::TextureFormat::RGBA8Unorm;
        case TextureFormat::RGBA16Float:    return wgpu::TextureFormat::RGBA16Float;
        case TextureFormat::RGBA32Float:    return wgpu::TextureFormat::RGBA32Float;
        case TextureFormat::R8Unorm:        return wgpu::TextureFormat::R8Unorm;
        case TextureFormat::R16Float:       return wgpu::TextureFormat::R16Float;
        case TextureFormat::BGRA8Unorm:     return wgpu::TextureFormat::BGRA8Unorm;
        case TextureFormat::BGRA8UnormSrgb: return wgpu::TextureFormat::BGRA8UnormSrgb;
        case TextureFormat::RGBA8UnormSrgb: return wgpu::TextureFormat::RGBA8UnormSrgb;
    }
    return wgpu::TextureFormat::RGBA16Float;
}

TextureFormat fromWgpu(wgpu::TextureFormat format) noexcept {
    switch (format) {
        case wgpu::TextureFormat::RGBA8Unorm:     return TextureFormat::RGBA8Unorm;
        case wgpu::TextureFormat::RGBA16Float:    return TextureFormat::RGBA16Float;
        case wgpu::TextureFormat::RGBA32Float:    return TextureFormat::RGBA32Float;
        case wgpu::TextureFormat::R8Unorm:        return TextureFormat::R8Unorm;
        case wgpu::TextureFormat::R16Float:       return TextureFormat::R16Float;
        case wgpu::TextureFormat::BGRA8UnormSrgb: return TextureFormat::BGRA8UnormSrgb;
        case wgpu::TextureFormat::RGBA8UnormSrgb: return TextureFormat::RGBA8UnormSrgb;
        default:                                  return TextureFormat::BGRA8Unorm;
    }
}

wgpu::TextureUsage toWgpu(TextureUsage usage) noexcept {
    wgpu::TextureUsage out = wgpu::TextureUsage::None;
    if (has(usage, TextureUsage::Sampled))  out |= wgpu::TextureUsage::TextureBinding;
    if (has(usage, TextureUsage::Storage))  out |= wgpu::TextureUsage::StorageBinding;
    if (has(usage, TextureUsage::RenderTo)) out |= wgpu::TextureUsage::RenderAttachment;
    if (has(usage, TextureUsage::CopySrc))  out |= wgpu::TextureUsage::CopySrc;
    if (has(usage, TextureUsage::CopyDst))  out |= wgpu::TextureUsage::CopyDst;
    return out;
}

std::string toString(wgpu::StringView view) {
    return (view.data == nullptr) ? std::string{} : std::string(view.data, view.length);
}

const char* backendName(wgpu::BackendType backend) noexcept {
    switch (backend) {
        case wgpu::BackendType::Metal:    return "Metal";
        case wgpu::BackendType::D3D12:    return "D3D12";
        case wgpu::BackendType::D3D11:    return "D3D11";
        case wgpu::BackendType::Vulkan:   return "Vulkan";
        case wgpu::BackendType::OpenGL:   return "OpenGL";
        case wgpu::BackendType::OpenGLES: return "OpenGLES";
        case wgpu::BackendType::WebGPU:   return "WebGPU";
        case wgpu::BackendType::Null:     return "Null";
        case wgpu::BackendType::Undefined: break;
    }
    return "Unknown";
}

// --- Resources ---------------------------------------------------------------

class DawnTexture final : public Texture {
public:
    DawnTexture(wgpu::Texture texture, const TextureDesc& desc)
        : texture_(std::move(texture)), desc_(desc) {}

    [[nodiscard]] std::uint32_t width() const noexcept override { return desc_.width; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return desc_.height; }
    [[nodiscard]] TextureFormat format() const noexcept override { return desc_.format; }

    [[nodiscard]] const wgpu::Texture& handle() const noexcept { return texture_; }

private:
    wgpu::Texture texture_;
    TextureDesc desc_;
};

// Wraps a swapchain backbuffer without owning it; valid only for the frame it was
// acquired for.
class SwapchainTexture final : public Texture {
public:
    SwapchainTexture(wgpu::Texture texture, std::uint32_t w, std::uint32_t h,
                     TextureFormat format)
        : texture_(std::move(texture)), width_(w), height_(h), format_(format) {}

    [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }
    [[nodiscard]] TextureFormat format() const noexcept override { return format_; }
    [[nodiscard]] const wgpu::Texture& handle() const noexcept { return texture_; }

private:
    wgpu::Texture texture_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    TextureFormat format_ = TextureFormat::BGRA8Unorm;
};

class DawnBuffer final : public Buffer {
public:
    DawnBuffer(wgpu::Buffer buffer, std::size_t bytes)
        : buffer_(std::move(buffer)), size_(bytes) {}

    [[nodiscard]] std::size_t size() const noexcept override { return size_; }
    [[nodiscard]] const wgpu::Buffer& handle() const noexcept { return buffer_; }

private:
    wgpu::Buffer buffer_;
    std::size_t size_ = 0;
};

// --- Command recorder --------------------------------------------------------

class DawnRenderPipeline final : public RenderPipeline {
public:
    explicit DawnRenderPipeline(wgpu::RenderPipeline pipeline)
        : pipeline_(std::move(pipeline)) {}
    [[nodiscard]] const wgpu::RenderPipeline& handle() const noexcept { return pipeline_; }

private:
    wgpu::RenderPipeline pipeline_;
};

class DawnRecorder final : public CommandRecorder {
public:
    DawnRecorder(wgpu::Device device, wgpu::Sampler sampler, std::string_view label)
        : device_(std::move(device)), sampler_(std::move(sampler)) {
        wgpu::CommandEncoderDescriptor desc{};
        desc.label = wgpu::StringView(label.data(), label.size());
        encoder_ = device_.CreateCommandEncoder(&desc);
    }

    void dispatch(const ComputePipelineHandle&, std::uint32_t, std::uint32_t) override {
        // Compute needs a compiled pipeline, which needs Slang. See create_compute_pipeline.
    }

    void bind_texture(std::uint32_t, const TextureHandle&) override {}
    void bind_storage_texture(std::uint32_t, const TextureHandle&) override {}
    void bind_uniforms(std::uint32_t, const BufferHandle&) override {}

    void begin_pass(const TextureHandle& target, float r, float g, float b,
                    float a) override {
        const wgpu::Texture* texture = wgpuTextureOf(target);
        if (texture == nullptr) {
            return;
        }
        wgpu::RenderPassColorAttachment attachment{};
        attachment.view = texture->CreateView();
        attachment.loadOp = wgpu::LoadOp::Clear;
        attachment.storeOp = wgpu::StoreOp::Store;
        attachment.clearValue = {static_cast<double>(r), static_cast<double>(g),
                                 static_cast<double>(b), static_cast<double>(a)};

        wgpu::RenderPassDescriptor desc{};
        desc.colorAttachmentCount = 1;
        desc.colorAttachments = &attachment;
        pass_ = encoder_.BeginRenderPass(&desc);
    }

    void end_pass() override {
        if (pass_ != nullptr) {
            pass_.End();
            pass_ = nullptr;
        }
    }

    void set_scissor(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                     std::uint32_t height) override {
        if (pass_ == nullptr) {
            return;
        }
        pass_.SetScissorRect(x, y, width, height);
    }

    void draw(const RenderPipelineHandle& pipeline, const BufferHandle& uniforms,
              const TextureHandle& texture, std::uint32_t vertex_count) override {
        const auto* dawnPipeline = dynamic_cast<const DawnRenderPipeline*>(pipeline.get());
        const auto* dawnBuffer = dynamic_cast<const DawnBuffer*>(uniforms.get());
        const wgpu::Texture* wgpuTexture = wgpuTextureOf(texture);
        if (pass_ == nullptr || dawnPipeline == nullptr || dawnBuffer == nullptr ||
            wgpuTexture == nullptr) {
            return;
        }

        wgpu::BindGroupEntry entries[3]{};
        entries[0].binding = 0;
        entries[0].buffer = dawnBuffer->handle();
        entries[0].size = dawnBuffer->size();
        entries[1].binding = 1;
        entries[1].sampler = sampler_;
        entries[2].binding = 2;
        entries[2].textureView = wgpuTexture->CreateView();

        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = dawnPipeline->handle().GetBindGroupLayout(0);
        bgDesc.entryCount = 3;
        bgDesc.entries = entries;

        pass_.SetPipeline(dawnPipeline->handle());
        pass_.SetBindGroup(0, device_.CreateBindGroup(&bgDesc));
        pass_.Draw(vertex_count);
    }

    void copy_texture(const TextureHandle& src, const TextureHandle& dst) override {
        const auto* from = dynamic_cast<const DawnTexture*>(src.get());
        const auto* to = dynamic_cast<const DawnTexture*>(dst.get());
        if (from == nullptr || to == nullptr) {
            return;
        }
        wgpu::TexelCopyTextureInfo source{};
        source.texture = from->handle();
        wgpu::TexelCopyTextureInfo destination{};
        destination.texture = to->handle();
        wgpu::Extent3D extent{from->width(), from->height(), 1};
        encoder_.CopyTextureToTexture(&source, &destination, &extent);
    }

    [[nodiscard]] wgpu::CommandBuffer finish() { return encoder_.Finish(); }

    // Both texture kinds carry a wgpu::Texture; the difference is only who owns it.
    static const wgpu::Texture* wgpuTextureOf(const TextureHandle& handle) {
        if (const auto* owned = dynamic_cast<const DawnTexture*>(handle.get())) {
            return &owned->handle();
        }
        if (const auto* borrowed = dynamic_cast<const SwapchainTexture*>(handle.get())) {
            return &borrowed->handle();
        }
        return nullptr;
    }

private:
    wgpu::Device device_;
    wgpu::Sampler sampler_;
    wgpu::CommandEncoder encoder_;
    wgpu::RenderPassEncoder pass_;
};

// --- Surface -----------------------------------------------------------------

class DawnSurface final : public Surface {
public:
    DawnSurface(wgpu::Surface surface, wgpu::Device device, wgpu::TextureFormat format)
        : surface_(std::move(surface)), device_(std::move(device)), format_(format) {}

    void configure(std::uint32_t width, std::uint32_t height) override {
        if (width == 0 || height == 0) {
            return;  // a collapsed splitter produces a zero-sized widget
        }
        if (width == width_ && height == height_) {
            return;
        }
        width_ = width;
        height_ = height;

        wgpu::SurfaceConfiguration config{};
        config.device = device_;
        config.format = format_;
        config.usage = wgpu::TextureUsage::RenderAttachment;
        config.width = width;
        config.height = height;
        config.presentMode = wgpu::PresentMode::Fifo;
        config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
        surface_.Configure(&config);
        configured_ = true;
    }

    TextureHandle acquire() override {
        if (!configured_) {
            return nullptr;
        }
        wgpu::SurfaceTexture current{};
        surface_.GetCurrentTexture(&current);
        if (current.texture == nullptr) {
            return nullptr;
        }
        return std::make_shared<SwapchainTexture>(current.texture, width_, height_,
                                                  fromWgpu(format_));
    }

    void present() override {
        if (configured_) {
            surface_.Present();
        }
    }

    [[nodiscard]] TextureFormat format() const noexcept override {
        return fromWgpu(format_);
    }

private:
    wgpu::Surface surface_;
    wgpu::Device device_;
    wgpu::TextureFormat format_ = wgpu::TextureFormat::BGRA8Unorm;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool configured_ = false;
};

// --- Device ------------------------------------------------------------------

class DawnDevice final : public GpuDevice {
public:
    DawnDevice(wgpu::Instance instance, wgpu::Adapter adapter, wgpu::Device device,
               std::string description)
        : instance_(std::move(instance)),
          adapter_(std::move(adapter)),
          device_(std::move(device)),
          queue_(device_.GetQueue()),
          description_(std::move(description)) {
        // One sampler for everything — linear filtering + clamped edges covers every
        // 2D op we do so far.
        wgpu::SamplerDescriptor sd{};
        sd.magFilter = wgpu::FilterMode::Linear;
        sd.minFilter = wgpu::FilterMode::Linear;
        sd.addressModeU = wgpu::AddressMode::ClampToEdge;
        sd.addressModeV = wgpu::AddressMode::ClampToEdge;
        sampler_ = device_.CreateSampler(&sd);
    }

    SurfaceHandle create_surface(void* native_window) override {
        void* layer = prepareNativeSurface(native_window);
        if (layer == nullptr) {
            return nullptr;
        }

        wgpu::SurfaceSourceMetalLayer fromLayer{};
        fromLayer.layer = layer;

        wgpu::SurfaceDescriptor desc{};
        desc.nextInChain = &fromLayer;
        wgpu::Surface surface = instance_.CreateSurface(&desc);
        if (surface == nullptr) {
            return nullptr;
        }

        // Uses whatever format the platform prefers, favoring sRGB — hardware applies
        // the linear-to-display transform on write instead of every shader by hand.
        wgpu::SurfaceCapabilities caps{};
        surface.GetCapabilities(adapter_, &caps);

        wgpu::TextureFormat format = wgpu::TextureFormat::BGRA8Unorm;
        if (caps.formatCount > 0) {
            format = caps.formats[0];
            for (std::size_t i = 0; i < caps.formatCount; ++i) {
                if (caps.formats[i] == wgpu::TextureFormat::BGRA8UnormSrgb ||
                    caps.formats[i] == wgpu::TextureFormat::RGBA8UnormSrgb) {
                    format = caps.formats[i];
                    break;
                }
            }
        }

        return std::make_shared<DawnSurface>(std::move(surface), device_, format);
    }

    TextureHandle create_texture(const TextureDesc& desc) override {
        wgpu::TextureDescriptor td{};
        td.label = wgpu::StringView(desc.debug_label.data(), desc.debug_label.size());
        td.dimension = wgpu::TextureDimension::e2D;
        td.size = {desc.width, desc.height, 1};
        td.format = toWgpu(desc.format);
        td.usage = toWgpu(desc.usage);
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        return std::make_shared<DawnTexture>(device_.CreateTexture(&td), desc);
    }

    BufferHandle create_uniform_buffer(std::size_t bytes, std::string_view label) override {
        wgpu::BufferDescriptor bd{};
        bd.label = wgpu::StringView(label.data(), label.size());
        bd.size = bytes;
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        return std::make_shared<DawnBuffer>(device_.CreateBuffer(&bd), bytes);
    }

    void write_buffer(const BufferHandle& dst, const void* data, std::size_t bytes) override {
        const auto* buffer = dynamic_cast<const DawnBuffer*>(dst.get());
        if (buffer == nullptr) {
            return;
        }
        queue_.WriteBuffer(buffer->handle(), 0, data, bytes);
    }

    void write_texture(const TextureHandle& dst, const void* data, std::size_t bytes,
                       std::uint32_t row_stride) override {
        const auto* texture = dynamic_cast<const DawnTexture*>(dst.get());
        if (texture == nullptr) {
            return;
        }
        wgpu::TexelCopyTextureInfo destination{};
        destination.texture = texture->handle();

        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = row_stride;
        layout.rowsPerImage = texture->height();

        wgpu::Extent3D extent{texture->width(), texture->height(), 1};
        queue_.WriteTexture(&destination, data, bytes, &layout, &extent);
    }

    RenderPipelineHandle create_render_pipeline(std::string_view wgsl,
                                                std::string_view vertex_entry,
                                                std::string_view fragment_entry,
                                                TextureFormat target_format,
                                                std::string_view label,
                                                BlendPreset blend_preset) override {
        wgpu::ShaderSourceWGSL source{};
        source.code = wgpu::StringView(wgsl.data(), wgsl.size());

        wgpu::ShaderModuleDescriptor moduleDesc{};
        moduleDesc.nextInChain = &source;
        moduleDesc.label = wgpu::StringView(label.data(), label.size());
        wgpu::ShaderModule module = device_.CreateShaderModule(&moduleDesc);

        // Source color is PREMULTIPLIED, so these compose as plain blend equations.
        // Alpha handling is identical across modes; only the color factors differ.
        wgpu::BlendState blend{};
        blend.alpha.operation = wgpu::BlendOperation::Add;
        blend.alpha.srcFactor = wgpu::BlendFactor::One;
        blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
        blend.color.operation = wgpu::BlendOperation::Add;

        switch (blend_preset) {
            case BlendPreset::AlphaOver:
                // s + d*(1-a); equivalent to straight-alpha SrcAlpha/OneMinusSrcAlpha
                // with premultiplied source.
                blend.color.srcFactor = wgpu::BlendFactor::One;
                blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
                break;
            case BlendPreset::Add:
                // s + d; premultiplication makes a half-transparent layer add half as
                // much, not all of it.
                blend.color.srcFactor = wgpu::BlendFactor::One;
                blend.color.dstFactor = wgpu::BlendFactor::One;
                break;
            case BlendPreset::Screen:
                // s + d*(1-s) == 1-(1-s)(1-d), the definition of screen.
                blend.color.srcFactor = wgpu::BlendFactor::One;
                blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrc;
                break;
            case BlendPreset::Multiply:
                // s*d + d*(1-a); the second term stops transparent parts of the layer
                // multiplying the backdrop to black.
                blend.color.srcFactor = wgpu::BlendFactor::Dst;
                blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
                break;
            case BlendPreset::Lighten:
                blend.color.operation = wgpu::BlendOperation::Max;
                blend.color.srcFactor = wgpu::BlendFactor::One;
                blend.color.dstFactor = wgpu::BlendFactor::One;
                break;
            case BlendPreset::Darken:
                // Min against premultiplied source treats transparent areas as 0,
                // darkening to black — only correct where the layer is opaque (same
                // caveat AE has).
                blend.color.operation = wgpu::BlendOperation::Min;
                blend.color.srcFactor = wgpu::BlendFactor::One;
                blend.color.dstFactor = wgpu::BlendFactor::One;
                break;
        }

        wgpu::ColorTargetState target{};
        target.format = toWgpu(target_format);
        target.blend = &blend;
        target.writeMask = wgpu::ColorWriteMask::All;

        wgpu::FragmentState fragment{};
        fragment.module = module;
        fragment.entryPoint = wgpu::StringView(fragment_entry.data(), fragment_entry.size());
        fragment.targetCount = 1;
        fragment.targets = &target;

        wgpu::RenderPipelineDescriptor desc{};
        desc.label = wgpu::StringView(label.data(), label.size());
        desc.vertex.module = module;
        desc.vertex.entryPoint = wgpu::StringView(vertex_entry.data(), vertex_entry.size());
        desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        desc.fragment = &fragment;

        return std::make_shared<DawnRenderPipeline>(device_.CreateRenderPipeline(&desc));
    }

    ComputePipelineHandle create_compute_pipeline(std::string_view, std::string_view) override {
        // Unimplemented — Slang toolchain not wired yet. Null rather than a stub so
        // callers can't mistake a no-op for a working effect.
        return nullptr;
    }

    std::unique_ptr<CommandRecorder> begin_commands(std::string_view label) override {
        return std::make_unique<DawnRecorder>(device_, sampler_, label);
    }

    void submit(std::unique_ptr<CommandRecorder> recorder) override {
        auto* dawn = dynamic_cast<DawnRecorder*>(recorder.get());
        if (dawn == nullptr) {
            return;
        }
        wgpu::CommandBuffer commands = dawn->finish();
        queue_.Submit(1, &commands);
    }

    void wait_idle() override {
        const wgpu::Future done = queue_.OnSubmittedWorkDone(
            wgpu::CallbackMode::WaitAnyOnly, [](wgpu::QueueWorkDoneStatus) {});
        instance_.WaitAny(done, UINT64_MAX);
    }

    [[nodiscard]] std::string description() const override { return description_; }

private:
    wgpu::Instance instance_;
    wgpu::Adapter adapter_;
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::Sampler sampler_;
    std::string description_;
};

}  // namespace

std::unique_ptr<GpuDevice> create_dawn_device() {
    // Timed waits are opt-in; without this, WaitAny with a timeout fails and adapter
    // creation silently returns nothing.
    wgpu::InstanceDescriptor instanceDesc{};
    instanceDesc.capabilities.timedWaitAnyEnable = true;
    instanceDesc.capabilities.timedWaitAnyMaxCount = 8;

    wgpu::Instance instance = wgpu::CreateInstance(&instanceDesc);
    if (instance == nullptr) {
        return nullptr;
    }

    wgpu::RequestAdapterOptions adapterOptions{};
    adapterOptions.powerPreference = wgpu::PowerPreference::HighPerformance;

    wgpu::Adapter adapter;
    wgpu::Future adapterRequest = instance.RequestAdapter(
        &adapterOptions, wgpu::CallbackMode::WaitAnyOnly,
        [&adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter found,
                   wgpu::StringView message) {
            if (status == wgpu::RequestAdapterStatus::Success) {
                adapter = std::move(found);
            } else {
                std::fprintf(stderr, "gpu: no adapter (%s)\n",
                             toString(message).c_str());
            }
        });
    instance.WaitAny(adapterRequest, UINT64_MAX);
    if (adapter == nullptr) {
        return nullptr;
    }

    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    const std::string description = toString(info.device) + " (" +
                                    backendName(info.backendType) + ", " +
                                    toString(info.vendor) + ")";

    wgpu::DeviceDescriptor deviceDesc{};
    deviceDesc.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
            std::fprintf(stderr, "gpu error (%d): %s\n", static_cast<int>(type),
                         toString(message).c_str());
        });
    deviceDesc.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
            // Destroyed fires on every normal shutdown; only other reasons indicate
            // an actual problem.
            if (reason == wgpu::DeviceLostReason::Destroyed) {
                return;
            }
            std::fprintf(stderr, "gpu device lost: %s\n", toString(message).c_str());
        });

    wgpu::Device device;
    wgpu::Future deviceRequest = adapter.RequestDevice(
        &deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
        [&device](wgpu::RequestDeviceStatus status, wgpu::Device found,
                  wgpu::StringView message) {
            if (status == wgpu::RequestDeviceStatus::Success) {
                device = std::move(found);
            } else {
                std::fprintf(stderr, "gpu: no device (%s)\n", toString(message).c_str());
            }
        });
    instance.WaitAny(deviceRequest, UINT64_MAX);
    if (device == nullptr) {
        return nullptr;
    }

    return std::make_unique<DawnDevice>(std::move(instance), std::move(adapter),
                                        std::move(device), description);
}

}  // namespace ruby::gpu
