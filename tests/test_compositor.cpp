// Renders every layer kind and blend mode through a real device. No GPU readback, so
// this catches crashes/validation failures, not pixel correctness. Reports 77 with no adapter.

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "ruby/engine/Compositor.h"
#include "ruby/gpu/GpuDevice.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

gpu::TextureHandle makeTarget(gpu::GpuDevice& device) {
    gpu::TextureDesc desc;
    desc.width = 320;
    desc.height = 568;
    desc.format = gpu::TextureFormat::RGBA16Float;
    desc.usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::RenderTo;
    desc.debug_label = "compositor test target";
    return device.create_texture(desc);
}

}  // namespace

int main() {
    std::unique_ptr<gpu::GpuDevice> device = gpu::create_dawn_device();
    if (device == nullptr) {
        std::puts("no adapter; skipping");
        return 77;
    }
    std::printf("adapter: %s\n", device->description().c_str());

    const gpu::TextureHandle target = makeTarget(*device);
    check(target != nullptr, "a render target");
    if (target == nullptr) {
        return EXIT_FAILURE;
    }

    engine::Compositor compositor(*device, gpu::TextureFormat::RGBA16Float);

    // One of every layer kind.
    core::Project project;
    core::Composition& comp = project.addComposition("t", 1080, 1920, 30.0, 10.0);

    const auto add = [&](const char* name, core::LayerKind kind) -> core::LayerId {
        // addLayer inserts at front; never hold a Layer& across another call.
        const core::LayerId id = project.addLayer(comp, name, kind).id;
        core::Layer* l = comp.find(id);
        l->inPoint = core::TimeValue::seconds(0.0);
        l->outPoint = core::TimeValue::seconds(10.0);
        return id;
    };

    const core::LayerId nullId = add("null", core::LayerKind::Null);
    const core::LayerId solidId = add("solid", core::LayerKind::Solid);
    const core::LayerId textId = add("text", core::LayerKind::Text);
    add("audio", core::LayerKind::Audio);
    add("footage with no media", core::LayerKind::Footage);
    add("precomp with no source", core::LayerKind::Precomp);

    comp.find(solidId)->solidColor = core::Value::rgba(0.2, 0.4, 0.8, 1.0);
    comp.find(textId)->text = "Hello";

    comp.find(solidId)->parent = nullId;

    // Stands in for the texture the UI would rasterise text into.
    gpu::TextureDesc textDesc;
    textDesc.width = 64;
    textDesc.height = 32;
    textDesc.format = gpu::TextureFormat::RGBA8Unorm;
    textDesc.usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst;
    textDesc.debug_label = "fake text";
    const gpu::TextureHandle textTexture = device->create_texture(textDesc);
    check(textTexture != nullptr, "a stand-in text texture");

    engine::Compositor::ExternalTextures external;
    external.emplace(textId, engine::Compositor::External{textTexture, 64, 32});

    // Exercises the per-mode pipeline cache, including unimplemented modes' fallback.
    const core::BlendMode modes[] = {
        core::BlendMode::Normal,    core::BlendMode::Add,
        core::BlendMode::Screen,    core::BlendMode::Multiply,
        core::BlendMode::Lighten,   core::BlendMode::Darken,
        core::BlendMode::Overlay,   core::BlendMode::SoftLight,
        core::BlendMode::HardLight, core::BlendMode::Difference};
    for (const core::BlendMode mode : modes) {
        comp.find(solidId)->blend = mode;
        compositor.render(project, comp, 1.0, target, &external);
    }
    device->wait_idle();
    check(true, "every blend mode rendered without a crash");

    // Solid sized explicitly rather than following the composition.
    comp.find(solidId)->solidWidth = 300;
    comp.find(solidId)->solidHeight = 80;
    compositor.render(project, comp, 1.0, target, &external);

    // No external textures: the export path's shape (text with nobody to rasterise it).
    compositor.render(project, comp, 1.0, target, nullptr);

    // Stale external entry for a layer no longer in the composition (post-delete cache).
    engine::Compositor::ExternalTextures stale;
    stale.emplace(9999, engine::Compositor::External{textTexture, 64, 32});
    compositor.render(project, comp, 1.0, target, &stale);

    // Rotation and off-centre anchor, previously untested through the render path.
    if (core::Property* rot = comp.find(solidId)->find("rotation"); rot != nullptr) {
        rot->staticValue = core::Value::scalar(37.0);
    }
    if (core::Property* anchor = comp.find(solidId)->find("anchor_point");
        anchor != nullptr) {
        anchor->staticValue = core::Value::vec2(50.0, -25.0);
    }
    compositor.render(project, comp, 1.0, target, &external);

    // Parent cycle: UI can't produce this yet, but unbounded it would hang, not misrender.
    comp.find(nullId)->parent = solidId;  // solid -> null -> solid
    compositor.render(project, comp, 1.0, target, &external);
    device->wait_idle();
    check(true, "a parent cycle renders instead of hanging");
    comp.find(nullId)->parent.reset();

    // Solo: nothing soloed draws everything, one soloed layer draws only it.
    comp.find(solidId)->solo = true;
    compositor.render(project, comp, 1.0, target, &external);
    comp.find(textId)->solo = true;
    compositor.render(project, comp, 1.0, target, &external);

    // Soloed and hidden at once, which is the case where two switches disagree.
    comp.find(solidId)->enabled = false;
    compositor.render(project, comp, 1.0, target, &external);
    comp.find(solidId)->enabled = true;
    comp.find(solidId)->solo = false;
    comp.find(textId)->solo = false;

    // Times outside every layer's span, and exactly on the boundaries.
    for (const double t : {0.0, 5.0, 9.999, 10.0, 25.0}) {
        compositor.render(project, comp, t, target, &external);
    }

    // A composition with no layers at all.
    core::Composition& bare = project.addComposition("bare", 1080, 1920, 30.0, 5.0);
    compositor.render(project, bare, 0.0, target, &external);

    device->wait_idle();
    check(true, "edge cases rendered without a crash");

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("compositor: all checks passed");
    return EXIT_SUCCESS;
}
