// Render graph hashing: a false-positive miss just costs perf, a false-positive hit shows
// the wrong frame silently.

#include <cstdio>
#include <string>

#include "ruby/engine/RenderGraph.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

struct Scene {
    core::Project project;
    core::Composition* comp = nullptr;
    core::LayerId a = 0;
    core::LayerId b = 0;

    Scene() {
        comp = &project.addComposition("c", 1080, 1920, 30.0, 10.0);
        a = project.addLayer(*comp, "back", core::LayerKind::Solid).id;
        b = project.addLayer(*comp, "front", core::LayerKind::Solid).id;
        for (const core::LayerId id : {a, b}) {
            comp->find(id)->outPoint = core::TimeValue::seconds(10.0);
        }
    }

    core::Layer& layer(core::LayerId id) { return *comp->find(id); }
    engine::RenderGraph at(double t) { return engine::buildGraph(project, *comp, t); }
    engine::NodeHash hashAt(double t) { return at(t).rootHash(); }
};

void the_same_frame_hashes_the_same_twice() {
    Scene s;
    check(s.hashAt(1.0) == s.hashAt(1.0), "building twice gives the same hash");
    check(s.hashAt(1.0) != 0, "and it is not trivially zero");
}

// Time quantises to a frame index; a double-keyed cache would miss on float noise.
void times_inside_one_frame_are_the_same_frame() {
    Scene s;
    check(s.hashAt(1.0) == s.hashAt(1.0 + 1.0 / 90.0),
          "a third of a frame later is the same frame");
    check(s.at(1.0).frame == s.at(1.0 + 1.0 / 90.0).frame, "and reports the same index");
    check(s.at(1.0).frame + 1 == s.at(1.0 + 1.0 / 30.0).frame, "the next frame is next");

    // Identical-looking frames must hash identically even at different frame indices,
    // so static content renders once and is reused.
    check(s.hashAt(1.0) == s.hashAt(1.0 + 1.0 / 30.0),
          "nothing moved, so the next frame is the same picture");

    s.layer(s.b).find("opacity")->addKey(
        [] { core::Keyframe k; k.time = core::TimeValue::seconds(0.0);
             k.value = core::Value::scalar(0.0); return k; }(), s.comp->timeContext());
    s.layer(s.b).find("opacity")->addKey(
        [] { core::Keyframe k; k.time = core::TimeValue::seconds(4.0);
             k.value = core::Value::scalar(100.0); return k; }(), s.comp->timeContext());
    check(s.hashAt(1.0) != s.hashAt(1.0 + 1.0 / 30.0),
          "once something is animated, consecutive frames differ");
}

// Transform belongs to the composite, not the layer; moving a layer keeps its own hash.
void moving_a_layer_leaves_its_own_output_alone() {
    Scene s;
    const engine::RenderGraph before = s.at(1.0);
    const int node = before.outputFor(s.b);
    check(node >= 0, "the layer has an output node");
    const engine::NodeHash layerBefore = before.nodes[static_cast<std::size_t>(node)].hash;

    s.layer(s.b).find("position")->staticValue = core::Value::vec2(10.0, 90.0);

    const engine::RenderGraph after = s.at(1.0);
    const int node2 = after.outputFor(s.b);
    check(after.nodes[static_cast<std::size_t>(node2)].hash == layerBefore,
          "the layer's own pixels did not change, so its hash did not");
    check(after.rootHash() != before.rootHash(),
          "but the finished frame did, so the composite's did");
}

// An effect parameter change must invalidate the layer's output hash.
void changing_an_effect_parameter_changes_the_layer() {
    Scene s;
    core::EffectInstance fx;
    fx.effectId = "core.stylize.vignette";
    fx.schema = 1;
    core::Property amount;
    amount.key = "amount";
    amount.staticValue = core::Value::scalar(20.0);
    fx.params.push_back(amount);
    s.layer(s.b).effects.push_back(fx);

    const engine::RenderGraph before = s.at(1.0);
    const engine::NodeHash was =
        before.nodes[static_cast<std::size_t>(before.outputFor(s.b))].hash;

    s.layer(s.b).effects[0].params[0].staticValue = core::Value::scalar(80.0);

    const engine::RenderGraph after = s.at(1.0);
    check(after.nodes[static_cast<std::size_t>(after.outputFor(s.b))].hash != was,
          "a parameter change changes the layer's output");
}

// A disabled effect must not appear in the graph (it shouldn't cost a pass).
void a_disabled_effect_is_absent_from_the_graph() {
    Scene s;
    core::EffectInstance fx;
    fx.effectId = "core.stylize.vignette";
    fx.enabled = true;
    s.layer(s.b).effects.push_back(fx);
    const std::size_t withEffect = s.at(1.0).nodes.size();

    s.layer(s.b).effects[0].enabled = false;
    const engine::RenderGraph off = s.at(1.0);
    check(off.nodes.size() == withEffect - 1, "the node is gone, not just skipped");
    check(off.nodes[static_cast<std::size_t>(off.outputFor(s.b))].kind ==
              engine::RenderNode::Kind::Source,
          "and the layer's output is its source again");
}

// Each of these changes the picture in a way a naive key would miss.
void every_visible_change_moves_the_frame_hash() {
    {
        Scene s;
        const auto was = s.hashAt(1.0);
        s.layer(s.b).blend = core::BlendMode::Screen;
        check(s.hashAt(1.0) != was, "blend mode");
    }
    {
        Scene s;
        const auto was = s.hashAt(1.0);
        s.layer(s.b).find("opacity")->staticValue = core::Value::scalar(40.0);
        check(s.hashAt(1.0) != was, "opacity");
    }
    {
        Scene s;
        const auto was = s.hashAt(1.0);
        s.layer(s.b).enabled = false;
        check(s.hashAt(1.0) != was, "hiding a layer");
    }
    {
        Scene s;
        const auto was = s.hashAt(1.0);
        s.layer(s.a).solo = true;
        check(s.hashAt(1.0) != was, "soloing one, which hides the other");
    }
    {
        Scene s;
        const auto was = s.hashAt(1.0);
        s.layer(s.b).solidColor = core::Value::rgba(1.0, 0.0, 0.0, 1.0);
        check(s.hashAt(1.0) != was, "a solid's colour");
    }
    {
        Scene s;
        const auto was = s.hashAt(1.0);
        s.layer(s.b).outPoint = core::TimeValue::seconds(0.5);
        check(s.hashAt(1.0) != was, "trimming a layer out from under the playhead");
    }
    {
        Scene s;
        const auto was = s.hashAt(1.0);
        s.comp->width = 1920;
        check(s.hashAt(1.0) != was, "the composition's size");
    }
}

// Stacking order must be part of the composite's hash, not just the set of layers.
void restacking_changes_the_frame() {
    Scene s;
    // Layers must differ; swapping identical layers would legitimately hash the same.
    s.layer(s.a).solidColor = core::Value::rgba(1.0, 0.0, 0.0, 1.0);
    s.layer(s.b).solidColor = core::Value::rgba(0.0, 0.0, 1.0, 1.0);
    s.layer(s.b).blend = core::BlendMode::Screen;

    const auto was = s.hashAt(1.0);
    std::swap(s.comp->layers[0], s.comp->layers[1]);
    check(s.hashAt(1.0) != was, "swapping two layers changes the frame");
}

// An empty composition is still cacheable; it gets a node rather than an absent root.
void an_empty_composition_still_has_a_root() {
    core::Project project;
    core::Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 10.0);
    const engine::RenderGraph g = engine::buildGraph(project, comp, 0.0);
    check(g.root >= 0, "there is a root");
    check(g.rootHash() != 0, "with a real hash");
    check(g.nodes.size() == 1, "and nothing under it");
}

// External content (currently text) can't be hashed directly; caller supplies a key instead.
void an_external_key_reaches_the_frame_hash() {
    Scene s;
    engine::ExternalKeys keys;
    keys[s.b] = 1234;
    const auto with = engine::buildGraph(s.project, *s.comp, 1.0, &keys).rootHash();
    keys[s.b] = 5678;
    const auto changed = engine::buildGraph(s.project, *s.comp, 1.0, &keys).rootHash();
    check(with != changed, "re-rasterised text is a different frame");
}

// Not std::hash: it can vary between runs/versions, fatal for an on-disk cache.
void the_hash_is_stable_and_not_std_hash() {
    const engine::NodeHash a = engine::hashString("core.blur.gaussian", 1469598103934665603ULL);
    check(a == 5641052493809917232ULL,
          "FNV-1a of a known string, pinned so a library change cannot silently move it");
    check(engine::hashDouble(0.0, 7) == engine::hashDouble(-0.0, 7),
          "negative zero hashes as zero, or an identical value misses its own entry");
}

}  // namespace

int main() {
    the_same_frame_hashes_the_same_twice();
    times_inside_one_frame_are_the_same_frame();
    moving_a_layer_leaves_its_own_output_alone();
    changing_an_effect_parameter_changes_the_layer();
    a_disabled_effect_is_absent_from_the_graph();
    every_visible_change_moves_the_frame_hash();
    restacking_changes_the_frame();
    an_empty_composition_still_has_a_root();
    an_external_key_reaches_the_frame_hash();
    the_hash_is_stable_and_not_std_hash();

    if (failures == 0) {
        std::puts("rendergraph: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
