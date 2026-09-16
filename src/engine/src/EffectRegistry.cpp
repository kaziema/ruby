#include "ruby/engine/EffectRegistry.h"

#include <algorithm>
#include <string>
#include <cctype>

namespace ruby::engine {
namespace {

using core::ParamType;
using core::SpatialUnit;

// Shared prologue for every effect shader. Fullscreen triangle (3 verts, no diagonal
// seam) instead of a quad.
constexpr const char* kEffectPrologue = R"(
struct EffectUniforms {
    params : array<vec4<f32>, 8>,
};
@group(0) @binding(0) var<uniform> u    : EffectUniforms;
@group(0) @binding(1) var        samp : sampler;
@group(0) @binding(2) var        tex  : texture_2d<f32>;

struct VsOut {
    @builtin(position) position : vec4<f32>,
    @location(0)       uv       : vec2<f32>,
};

@vertex
fn vs(@builtin(vertex_index) index : u32) -> VsOut {
    var corners = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    var out : VsOut;
    out.position = vec4<f32>(corners[index], 0.0, 1.0);
    // Clip space is y-up, texture space is y-down.
    out.uv = vec2<f32>((corners[index].x + 1.0) * 0.5, (1.0 - corners[index].y) * 0.5);
    return out;
}

// Rec.709 luminance; valid since we work in linear light.
fn luma(c : vec3<f32>) -> f32 {
    return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
}
)";

// Range has no default: an unbounded param must be explicit (ParamRange::unbounded).
// All effects are schema 1; validate_against_previous enforces a version bump whenever
// a range tightens later.
core::ParamSpec param(std::string key, std::string label, SpatialUnit unit, double value,
                      int order, core::ParamRange range) {
    core::ParamSpec spec;
    spec.key = std::move(key);
    spec.label = std::move(label);
    spec.order = order;
    spec.type = ParamType::Float;
    spec.unit = unit;
    spec.default_value = value;
    spec.range = range;
    spec.introduced_in_schema = 1;
    return spec;
}

using R = core::ParamRange;

EffectDef makeGrade() {
    EffectDef def;
    def.schema.id = "core.color.grade";
    def.schema.schema = 1;
    def.schema.display_name = "Grade";
    def.schema.params = {
        param("exposure", "Exposure", SpatialUnit::Normalized, 0.0, 0,
              // Stops; unbounded, since a deliberate blowout past +/-6 is valid.
              R::unbounded(-6.0, 6.0)),
        param("contrast", "Contrast", SpatialUnit::Percent, 100.0, 1,
              // 0 = flat grey, negative inverts (a real look).
              R::unbounded(0.0, 300.0)),
        param("saturation", "Saturation", SpatialUnit::Percent, 100.0, 2,
              // Floored at 0: negative saturation is channel inversion, which has its
              // own effect.
              R::atLeast(0.0, 300.0)),
    };

    // Linear-light math throughout; contrast pivots on 18% grey (scene-referred mid),
    // not 0.5.
    def.shader = std::string(kEffectPrologue) + R"(
@fragment
fn fs(in : VsOut) -> @location(0) vec4<f32> {
    var c = textureSample(tex, samp, in.uv);

    let exposure   = u.params[0].x;
    let contrast   = u.params[1].x * 0.01;
    let saturation = u.params[2].x * 0.01;

    c = vec4<f32>(c.rgb * pow(2.0, exposure), c.a);
    c = vec4<f32>((c.rgb - vec3<f32>(0.18)) * contrast + vec3<f32>(0.18), c.a);
    c = vec4<f32>(mix(vec3<f32>(luma(c.rgb)), c.rgb, saturation), c.a);

    return vec4<f32>(max(c.rgb, vec3<f32>(0.0)), c.a);
}
)";
    return def;
}


// --- Effect library ------------------------------------------------------------

// Single-pass fixed-tap approximation. A separable two-pass blur would be faster but
// needs ping-pong buffers the effect stack doesn't expose yet.
EffectDef makeBlur() {
    EffectDef def;
    def.schema.id = "core.blur.gaussian";
    def.schema.schema = 1;
    def.schema.display_name = "Gaussian Blur";
    def.schema.params = {
        param("radius", "Radius", SpatialUnit::PercentOfWidth, 0.0, 0,
              // 9 taps/axis; past ~10% of frame width this bands instead of blurring.
              R::atLeast(0.0, 10.0)),
    };
    def.shader = std::string(kEffectPrologue) + R"(
@fragment
fn fs(in : VsOut) -> @location(0) vec4<f32> {
    let radius = u.params[0].x * 0.01;
    if (radius <= 0.0) {
        return textureSample(tex, samp, in.uv);
    }
    // Weight sum is divided out rather than hardcoded, so changing tap count can't
    // change exposure.
    var total = vec4<f32>(0.0);
    var weightSum = 0.0;
    for (var y = -4; y <= 4; y = y + 1) {
        for (var x = -4; x <= 4; x = x + 1) {
            let offset = vec2<f32>(f32(x), f32(y)) * radius * 0.25;
            let d = f32(x * x + y * y);
            let w = exp(-d / 8.0);
            total = total + textureSample(tex, samp, in.uv + offset) * w;
            weightSum = weightSum + w;
        }
    }
    return total / weightSum;
}
)";
    return def;
}

// Splits channels apart along an angle.
EffectDef makeChromatic() {
    EffectDef def;
    def.schema.id = "core.distort.chromatic";
    def.schema.schema = 1;
    def.schema.display_name = "Chromatic Aberration";
    def.schema.params = {
        param("amount", "Amount", SpatialUnit::PercentOfWidth, 0.0, 0,
              // Negative sign swaps R/B separation direction.
              R::unbounded(-5.0, 5.0)),
        param("angle", "Angle", SpatialUnit::Degrees, 0.0, 1,
              // Wraps; unbounded, slider covers one turn.
              R::unbounded(0.0, 360.0)),
    };
    def.shader = std::string(kEffectPrologue) + R"(
@fragment
fn fs(in : VsOut) -> @location(0) vec4<f32> {
    let amount = u.params[0].x * 0.01;
    let angle = radians(u.params[1].x);
    let dir = vec2<f32>(cos(angle), sin(angle)) * amount;

    // R/B shift opposite ways, G stays put, so the image doesn't appear to shift
    // overall; alpha samples center to avoid edge fringing.
    let r = textureSample(tex, samp, in.uv + dir).r;
    let g = textureSample(tex, samp, in.uv);
    let b = textureSample(tex, samp, in.uv - dir).b;
    return vec4<f32>(r, g.g, b, g.a);
}
)";
    return def;
}

// Bloom on bright areas; threshold in linear light isolates highlights.
EffectDef makeGlow() {
    EffectDef def;
    def.schema.id = "core.glow.bloom";
    def.schema.schema = 1;
    def.schema.display_name = "Glow";
    def.schema.params = {
        param("threshold", "Threshold", SpatialUnit::Normalized, 1.0, 0,
              // Above 1.0 = highlights, in linear light. Floored at 0 or everything glows.
              R::atLeast(0.0, 4.0)),
        param("radius", "Radius", SpatialUnit::PercentOfWidth, 2.0, 1,
              R::atLeast(0.0, 20.0)),
        param("intensity", "Intensity", SpatialUnit::Percent, 100.0, 2,
              R::atLeast(0.0, 400.0)),
    };
    def.shader = std::string(kEffectPrologue) + R"(
@fragment
fn fs(in : VsOut) -> @location(0) vec4<f32> {
    let threshold = u.params[0].x;
    let radius    = u.params[1].x * 0.01;
    let intensity = u.params[2].x * 0.01;

    let base = textureSample(tex, samp, in.uv);
    var bloom = vec3<f32>(0.0);
    var weightSum = 0.0;
    for (var y = -3; y <= 3; y = y + 1) {
        for (var x = -3; x <= 3; x = x + 1) {
            let offset = vec2<f32>(f32(x), f32(y)) * radius * 0.33;
            let s = textureSample(tex, samp, in.uv + offset).rgb;
            // Contribution scales with how far over the threshold.
            let over = max(s - vec3<f32>(threshold), vec3<f32>(0.0));
            let w = exp(-f32(x * x + y * y) / 6.0);
            bloom = bloom + over * w;
            weightSum = weightSum + w;
        }
    }
    return vec4<f32>(base.rgb + bloom / weightSum * intensity, base.a);
}
)";
    return def;
}

// Directional smear; cheap since it's one axis rather than a full kernel.
EffectDef makeMotionBlur() {
    EffectDef def;
    def.schema.id = "core.blur.directional";
    def.schema.schema = 1;
    def.schema.display_name = "Directional Blur";
    def.schema.params = {
        param("length", "Length", SpatialUnit::PercentOfWidth, 0.0, 0,
              R::atLeast(0.0, 15.0)),
        param("angle", "Angle", SpatialUnit::Degrees, 0.0, 1,
              // Wraps; unbounded, slider covers one turn.
              R::unbounded(0.0, 360.0)),
    };
    def.shader = std::string(kEffectPrologue) + R"(
@fragment
fn fs(in : VsOut) -> @location(0) vec4<f32> {
    let len = u.params[0].x * 0.01;
    let angle = radians(u.params[1].x);
    if (len <= 0.0) {
        return textureSample(tex, samp, in.uv);
    }
    let step = vec2<f32>(cos(angle), sin(angle)) * len / 12.0;
    var total = vec4<f32>(0.0);
    // Centered on the pixel, not trailing, so applying the effect doesn't shift the layer.
    for (var i = -6; i <= 6; i = i + 1) {
        total = total + textureSample(tex, samp, in.uv + step * f32(i));
    }
    return total / 13.0;
}
)";
    return def;
}

// Standard lift/gamma/gain color model.
EffectDef makeLiftGammaGain() {
    EffectDef def;
    def.schema.id = "core.color.liftgammagain";
    def.schema.schema = 1;
    def.schema.display_name = "Lift Gamma Gain";
    def.schema.params = {
        param("lift", "Lift", SpatialUnit::Normalized, 0.0, 0,
              // Added to the blacks; small numbers.
              R::unbounded(-0.5, 0.5)),
        param("gamma", "Gamma", SpatialUnit::Normalized, 1.0, 1,
              // Floored above 0: pow() would divide-by-zero or flip sign at <=0.
              R::atLeast(0.01, 4.0)),
        param("gain", "Gain", SpatialUnit::Normalized, 1.0, 2,
              R::atLeast(0.0, 4.0)),
    };
    def.shader = std::string(kEffectPrologue) + R"(
@fragment
fn fs(in : VsOut) -> @location(0) vec4<f32> {
    let lift  = u.params[0].x;
    let gamma = max(u.params[1].x, 0.01);
    let gain  = u.params[2].x;

    let c = textureSample(tex, samp, in.uv);
    var v = max(c.rgb + vec3<f32>(lift), vec3<f32>(0.0));
    v = pow(v, vec3<f32>(1.0 / gamma));
    v = v * gain;
    return vec4<f32>(max(v, vec3<f32>(0.0)), c.a);
}
)";
    return def;
}

// Darkens the corners.
EffectDef makeVignette() {
    EffectDef def;
    def.schema.id = "core.stylize.vignette";
    def.schema.schema = 1;
    def.schema.display_name = "Vignette";
    def.schema.params = {
        param("amount", "Amount", SpatialUnit::Percent, 0.0, 0,
              // Bounded 0-100: past 100 there's nothing left to darken.
              R::between(0.0, 100.0)),
        param("softness", "Softness", SpatialUnit::Percent, 50.0, 1,
              R::between(0.0, 100.0)),
    };
    def.shader = std::string(kEffectPrologue) + R"(
@fragment
fn fs(in : VsOut) -> @location(0) vec4<f32> {
    let amount = u.params[0].x * 0.01;
    let softness = max(u.params[1].x * 0.01, 0.01);

    let c = textureSample(tex, samp, in.uv);
    // Distance from center, normalized so corners are 1.
    let d = length(in.uv - vec2<f32>(0.5)) / 0.7071;
    let falloff = 1.0 - smoothstep(1.0 - softness, 1.0, d) * amount;
    return vec4<f32>(c.rgb * falloff, c.a);
}
)";
    return def;
}

// Quantizes color into hard steps.
EffectDef makePosterize() {
    EffectDef def;
    def.schema.id = "core.stylize.posterize";
    def.schema.schema = 1;
    def.schema.display_name = "Posterize";
    def.schema.params = {
        param("levels", "Levels", SpatialUnit::Normalized, 8.0, 0,
              // Below 1 step the math is meaningless; hard floor.
              R::atLeast(1.0, 32.0)),
    };
    def.shader = std::string(kEffectPrologue) + R"(
@fragment
fn fs(in : VsOut) -> @location(0) vec4<f32> {
    let levels = max(u.params[0].x, 2.0);
    let c = textureSample(tex, samp, in.uv);
    return vec4<f32>(floor(c.rgb * levels) / levels, c.a);
}
)";
    return def;
}

}  // namespace

std::string effectCategory(std::string_view id) {
    const std::size_t first = id.find('.');
    if (first == std::string_view::npos) {
        return "Other";
    }
    const std::size_t second = id.find('.', first + 1);
    if (second == std::string_view::npos) {
        return "Other";
    }
    std::string name(id.substr(first + 1, second - first - 1));
    if (!name.empty()) {
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    }
    return name;
}

EffectRegistry::EffectRegistry() {
    // Order here is the order they appear under each category in the menu.
    effects_.push_back(makeGrade());
    effects_.push_back(makeLiftGammaGain());
    effects_.push_back(makeBlur());
    effects_.push_back(makeMotionBlur());
    effects_.push_back(makeChromatic());
    effects_.push_back(makeGlow());
    effects_.push_back(makeVignette());
    effects_.push_back(makePosterize());
}

const EffectRegistry& EffectRegistry::instance() {
    static const EffectRegistry registry;
    return registry;
}

const EffectDef* EffectRegistry::find(std::string_view id) const noexcept {
    const auto it = std::find_if(effects_.begin(), effects_.end(),
                                 [id](const EffectDef& d) { return d.schema.id == id; });
    return it == effects_.end() ? nullptr : &*it;
}

EffectRegistryMigrationReport EffectRegistry::migrate(
    core::EffectInstance& instance) const {
    const EffectDef* def = find(instance.effectId);
    if (def == nullptr) {
        // Unknown effect; loader already reported it. Leave params untouched so a
        // future build that knows this effect can still read them.
        EffectRegistryMigrationReport report;
        report.ok = false;
        report.from_schema = instance.schema;
        report.to_schema = instance.schema;
        return report;
    }
    return migrateAgainst(*def, instance);
}

EffectRegistryMigrationReport migrateAgainst(const EffectDef& definition,
                                             core::EffectInstance& instance) {
    const EffectDef* def = &definition;
    EffectRegistryMigrationReport report;
    report.from_schema = instance.schema;
    report.to_schema = instance.schema;

    const int current = def->schema.schema;
    const int authored = instance.schema > 0 ? instance.schema : 1;

    if (authored > current) {
        // Newer-schema content is refused, not guessed at: reinterpreting unknown
        // params with today's schema would silently rewrite them.
        report.ok = false;
        report.notes.push_back(instance.effectId + " was authored at schema " +
                               std::to_string(authored) + " and this build only knows " +
                               std::to_string(current) +
                               "; its parameters were left untouched");
        return report;
    }

    // Bag = what the file actually had, regardless of current schema.
    core::ParamBag bag;
    for (const core::Property& p : instance.params) {
        core::ParamBag::Entry e;
        e.value = p.staticValue;
        e.keys = p.keys;
        e.expression = p.expression;
        bag.set(p.key, std::move(e));
    }

    if (authored < current) {
        core::runMigrations(def->migrations, authored, current, bag);
        report.notes.push_back(instance.effectId + " migrated from schema " +
                               std::to_string(authored) + " to " +
                               std::to_string(current));
    }

    // Rebuild param list in schema order from the bag; leftover bag entries are
    // retired or renamed keys.
    std::vector<core::Property> rebuilt;
    rebuilt.reserve(def->schema.params.size());

    for (const core::ParamSpec& spec : def->schema.params) {
        core::Property p;
        p.key = spec.key;
        p.label = spec.label;
        p.unit = spec.unit;
        p.range = spec.range;
        p.group = spec.group.empty() ? def->schema.display_name : spec.group;

        if (const core::ParamBag::Entry* e = bag.find(spec.key); e != nullptr) {
            p.staticValue = e->value;
            p.keys = e->keys;
            p.expression = e->expression;
            bag.remove(spec.key);
        } else if (spec.introduced_in_schema > authored && spec.legacy_default.has_value()) {
            // A param added after the content's schema takes legacy_default, not
            // default_value: a better default must never silently rewrite a saved look.
            // validate() requires every post-v1 param to have a legacy_default.
            p.staticValue = core::Value::scalar(*spec.legacy_default);
            report.notes.push_back(instance.effectId + "." + spec.key +
                                   " did not exist at schema " + std::to_string(authored) +
                                   "; filled in with its legacy default");
        } else {
            p.staticValue = core::Value::scalar(spec.default_value);
        }
        rebuilt.push_back(std::move(p));
    }

    for (const auto& [key, entry] : bag.entries()) {
        (void)entry;
        // Retired keys drop silently; unrecognized keys are reported (corrupt file or
        // migration bug).
        if (!def->schema.is_retired(key)) {
            report.notes.push_back(instance.effectId + " had an unknown parameter \"" +
                                   key + "\", which was dropped");
        }
    }

    instance.params = std::move(rebuilt);
    instance.schema = current;
    instance.displayName = def->schema.display_name;
    report.to_schema = current;
    return report;
}

void EffectRegistry::adoptSchema(core::EffectInstance& instance) const {
    const EffectDef* def = find(instance.effectId);
    if (def == nullptr) {
        return;  // unknown effect; the loader already reported it
    }
    for (core::Property& p : instance.params) {
        if (const core::ParamSpec* spec = def->schema.find(p.key); spec != nullptr) {
            p.range = spec->range;
            if (!spec->group.empty()) {
                p.group = spec->group;
            }
        }
    }
}

core::EffectInstance EffectRegistry::instantiate(std::string_view id) const {
    core::EffectInstance instance;
    const EffectDef* def = find(id);
    if (def == nullptr) {
        return instance;
    }

    instance.effectId = def->schema.id;
    instance.schema = def->schema.schema;
    instance.displayName = def->schema.display_name;

    for (const core::ParamSpec& spec : def->schema.params) {
        core::Property property;
        property.key = spec.key;
        property.label = spec.label;
        // Empty group means "use the effect's own name" (default before groups existed).
        property.group = spec.group.empty() ? def->schema.display_name : spec.group;
        property.unit = spec.unit;
        property.range = spec.range;
        property.staticValue = core::Value::scalar(spec.default_value);
        instance.params.push_back(std::move(property));
    }
    return instance;
}

}  // namespace ruby::engine
