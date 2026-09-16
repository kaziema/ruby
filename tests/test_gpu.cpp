// Brings up a real Dawn device against the actual adapter and submits real work.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ruby/engine/EffectRegistry.h"
#include "ruby/gpu/GpuDevice.h"

using namespace ruby::gpu;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main() {
    std::unique_ptr<GpuDevice> device = create_dawn_device();
    check(device != nullptr, "a GPU device comes up");
    if (device == nullptr) {
        std::fprintf(stderr, "\nno adapter available; the rest cannot run\n");
        return EXIT_FAILURE;
    }

    std::printf("adapter: %s\n", device->description().c_str());
    check(!device->description().empty(), "the adapter identifies itself");

    TextureDesc desc;
    desc.width = 1080;
    desc.height = 1920;
    desc.format = TextureFormat::RGBA16Float;
    desc.usage = TextureUsage::Sampled | TextureUsage::RenderTo | TextureUsage::CopySrc;
    desc.debug_label = "test target";

    TextureHandle target = device->create_texture(desc);
    check(target != nullptr, "a 1080x1920 RGBA16F texture allocates");
    if (target != nullptr) {
        check(target->width() == 1080, "texture reports its width");
        check(target->height() == 1920, "texture reports its height");
        check(target->format() == TextureFormat::RGBA16Float, "texture reports its format");
    }

    BufferHandle uniforms = device->create_uniform_buffer(256, "test uniforms");
    check(uniforms != nullptr, "a uniform buffer allocates");
    if (uniforms != nullptr) {
        check(uniforms->size() == 256, "buffer reports its size");
        const std::vector<float> values(64, 1.0f);
        device->write_buffer(uniforms, values.data(), values.size() * sizeof(float));
    }

    // Copy between two textures, then block until the GPU has actually done it.
    TextureDesc copyDesc = desc;
    copyDesc.usage = TextureUsage::CopyDst | TextureUsage::Sampled;
    copyDesc.debug_label = "test copy";
    TextureHandle copy = device->create_texture(copyDesc);
    check(copy != nullptr, "a second texture allocates");

    auto recorder = device->begin_commands("test submit");
    check(recorder != nullptr, "a command recorder opens");
    if (recorder != nullptr && target != nullptr && copy != nullptr) {
        recorder->copy_texture(target, copy);
        device->submit(std::move(recorder));
        device->wait_idle();
    }

    // Every blend preset must build; a rejected pipeline shows up as silent wrong-mode
    // drawing, not an error.
    {
        constexpr const char* kShader = R"(
@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {
    var p = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    return vec4<f32>(p[i], 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4<f32> { return vec4<f32>(1.0, 1.0, 1.0, 1.0); }
)";
        struct Case { BlendPreset preset; const char* name; };
        const Case cases[] = {
            {BlendPreset::AlphaOver, "alpha over"}, {BlendPreset::Add, "add"},
            {BlendPreset::Screen, "screen"},        {BlendPreset::Multiply, "multiply"},
            {BlendPreset::Lighten, "lighten"},      {BlendPreset::Darken, "darken"},
        };
        for (const Case& c : cases) {
            const RenderPipelineHandle pipeline = device->create_render_pipeline(
                kShader, "vs", "fs", TextureFormat::RGBA16Float, c.name, c.preset);
            if (pipeline == nullptr) {
                std::fprintf(stderr, "FAIL: blend preset '%s' did not build\n", c.name);
                ++failures;
            }
        }
    }

    // A shader that fails to compile yields a null pipeline the compositor silently
    // skips — the effect appears fully functional but never changes the picture.
    for (const ruby::engine::EffectDef& def : ruby::engine::EffectRegistry::instance().all()) {
        const RenderPipelineHandle pipeline = device->create_render_pipeline(
            def.shader, "vs", "fs", TextureFormat::RGBA16Float, def.schema.id);
        if (pipeline == nullptr) {
            std::fprintf(stderr, "FAIL: effect '%s' does not compile\n",
                         def.schema.id.c_str());
            ++failures;
        }
    }
    std::printf("effects compiled: %zu\n",
                ruby::engine::EffectRegistry::instance().all().size());

    // Documented gap, asserted so it cannot be silently "fixed" into a no-op stub.
    check(device->create_compute_pipeline("noop", "main") == nullptr,
          "compute pipelines are not implemented yet and say so by returning null");

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("gpu: all checks passed");
    return EXIT_SUCCESS;
}
