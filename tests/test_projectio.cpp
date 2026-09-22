// Round trip must be lossless, and the loader must survive malformed real-world files.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ruby/core/Transform.h"
#include "ruby/io/ProjectIO.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

core::Project makeProject() {
    core::Project p;
    core::MediaItem& clip =
        p.addMedia("/clips/a.mp4", "a.mp4", core::MediaKind::Video, 12.5, 1920, 1080,
                   23.976, true);

    core::Composition& comp = p.addComposition("sneaker", 1080, 1920, 29.97, 14.5);
    comp.workIn = core::TimeValue::seconds(2.0);
    comp.workOut = core::TimeValue::seconds(9.0);
    comp.rhythm.setLane(core::MarkerLane::Vocal,
                        {{2.167, core::MarkerLane::Vocal, 0.84f, 0},
                         {3.083, core::MarkerLane::Vocal, 0.61f, 1}});
    comp.rhythm.addUserMarker(5.5);

    // Solid must be created first: addLayer inserts at front, so an earlier Layer& dangles.
    {
        core::Layer& solid = p.addLayer(comp, "Backdrop", core::LayerKind::Solid);
        solid.solidColor = core::Value::rgba(0.25, 0.5, 0.75, 1.0);
        solid.solidWidth = 640;
        solid.solidHeight = 0;  // 0 = follow the composition
    }

    core::Layer& layer = p.addLayer(comp, "a.mp4", core::LayerKind::Footage);
    layer.media = clip.id;
    layer.blend = core::BlendMode::Add;
    layer.inPoint = core::TimeValue::beats(2.0);
    layer.outPoint = core::TimeValue::seconds(9.25);
    layer.expanded = true;
    // Eye and speaker are separate switches; "visible but muted" must survive reload.
    layer.enabled = true;
    layer.audioEnabled = false;
    layer.locked = true;
    layer.transformExpanded = false;

    const core::TimeContext ctx = comp.timeContext();
    if (core::Property* pos = layer.find("position"); pos != nullptr) {
        pos->addKey({core::TimeValue::seconds(0.6), core::Value::vec2(50.0, 88.0),
                     core::Interpolation::Bezier, 0.68, 0.68, 0.12},
                    ctx);
        pos->addKey({core::TimeValue::beats(4.0), core::Value::vec2(50.0, 42.0),
                     core::Interpolation::Hold, 0.0, 0.0, 0.0},
                    ctx);
        pos->expression = "wiggle(30, 10)";
    }

    core::EffectInstance grade;
    grade.effectId = "core.color.grade";
    grade.schema = 1;
    grade.displayName = "Grade";
    core::Property sat;
    sat.key = "saturation";
    sat.label = "Saturation";
    sat.group = "Grade";
    sat.unit = core::SpatialUnit::Percent;
    sat.staticValue = core::Value::scalar(160.0);
    grade.params.push_back(sat);
    layer.effects.push_back(grade);

    return p;
}

// The loader must repair parent loops from hand edits, merges, or buggy writers.
void a_parent_loop_in_a_file_is_broken_on_load() {
    core::Project source;
    core::Composition& comp =
        source.addComposition("c", 1080, 1920, 30.0, 10.0);
    const core::LayerId a = source.addLayer(comp, "a", core::LayerKind::Null).id;
    const core::LayerId b = source.addLayer(comp, "b", core::LayerKind::Null).id;
    comp.find(a)->parent = b;
    comp.find(b)->parent = a;

    core::Project loaded;
    const io::LoadReport report = io::fromJson(loaded, io::toJson(source));
    check(report.ok, "a project with a parent loop still loads");
    check(!report.clean(), "but it is not reported as clean");
    check(!report.notes.empty(), "and says what it had to repair");

    const core::Composition& out = loaded.compositions().front();
    for (const core::Layer& layer : out.layers) {
        check(!core::hasParentCycle(out, layer.id), "no loop survives the load");
    }
}

// The ordinary case has to keep working: a valid parent must survive a round trip.
void a_valid_parent_survives_a_round_trip() {
    core::Project source;
    core::Composition& comp = source.addComposition("c", 1080, 1920, 30.0, 10.0);
    const core::LayerId parent = source.addLayer(comp, "null", core::LayerKind::Null).id;
    const core::LayerId child = source.addLayer(comp, "child", core::LayerKind::Solid).id;
    comp.find(child)->parent = parent;

    core::Project loaded;
    const io::LoadReport report = io::fromJson(loaded, io::toJson(source));
    check(report.clean(), "a project with ordinary parenting loads clean");

    const core::Composition& out = loaded.compositions().front();
    const auto at = std::find_if(out.layers.begin(), out.layers.end(),
                                 [](const core::Layer& l) { return l.name == "child"; });
    check(at != out.layers.end() && at->parent.has_value(), "the child kept its parent");
}

}  // namespace

// Masks round-trip exactly; a shape this build can't draw is reported, never guessed.
void masks_survive_a_round_trip() {
    core::Project source;
    core::Composition& comp = source.addComposition("c", 1080, 1920, 30.0, 10.0);
    core::Layer& layer = source.addLayer(comp, "l", core::LayerKind::Solid);

    core::EffectInstance fx;
    fx.effectId = "core.color.grade";
    fx.schema = 1;
    core::Mask first = core::makeMask(core::MaskShape::Ellipse, "Mask 1");
    first.mode = core::MaskMode::Subtract;
    first.inverted = true;
    first.find("feather")->staticValue = core::Value::scalar(7.5);
    first.find("center")->addKey({core::TimeValue::seconds(1.0), core::Value::vec2(20.0, 80.0),
                                  core::Interpolation::Linear, 0.0, 0.0, 0.0},
                                 comp.timeContext());
    fx.masks.push_back(first);
    fx.masks.push_back(core::makeMask(core::MaskShape::Rectangle, "Mask 2"));
    layer.effects.push_back(fx);

    core::Project loaded;
    const io::LoadReport report = io::fromJson(loaded, io::toJson(source));
    check(report.clean(), "a project with masks loads cleanly");
    const core::EffectInstance& back =
        loaded.compositions().front().layers.front().effects.front();
    check(back.masks.size() == 2, "both masks survive");
    if (back.masks.size() == 2) {
        const core::Mask& m = back.masks[0];
        check(m.name == "Mask 1" && m.shape == core::MaskShape::Ellipse,
              "name and shape survive");
        check(m.mode == core::MaskMode::Subtract && m.inverted, "mode and invert survive");
        check(m.find("feather")->staticValue.x() == 7.5, "a changed value survives");
        check(m.find("center")->keys.size() == 1, "keyframes survive");
        const core::Property* opacity = m.find("opacity");
        check(opacity->range.maximum.has_value() && *opacity->range.maximum == 100.0,
              "ranges come from the definition, not the file");
        check(back.masks[1].shape == core::MaskShape::Rectangle &&
                  back.masks[1].mode == core::MaskMode::Add,
              "the second mask keeps its own settings");
    }

    // A mask-less effect writes no masks key, so files without masks are unchanged.
    core::Project plain;
    core::Composition& plainComp = plain.addComposition("c", 1080, 1920, 30.0, 10.0);
    core::EffectInstance bare;
    bare.effectId = "core.color.grade";
    plain.addLayer(plainComp, "l", core::LayerKind::Solid).effects.push_back(bare);
    check(io::toJson(plain).find("\"masks\"") == std::string::npos,
          "an effect without masks writes no masks key");

    // A newer build's shape (say, a Bezier mask) is skipped with a note.
    std::string json = io::toJson(source);
    const std::string key = "\"shape\": \"ellipse\"";
    const auto at = json.find(key);
    check(at != std::string::npos, "the shape is actually written");
    if (at != std::string::npos) {
        json.replace(at, key.size(), "\"shape\": \"bezier\"");
        core::Project future;
        const io::LoadReport r = io::fromJson(future, json);
        check(r.ok, "a file with an unknown mask shape still loads");
        check(!r.notes.empty(), "and says it skipped something");
        const auto& masks = future.compositions().front().layers.front().effects.front().masks;
        check(masks.size() == 1 && masks[0].name == "Mask 2",
              "the unknown mask is skipped, not read as a rectangle");
    }
}

int main() {
    a_parent_loop_in_a_file_is_broken_on_load();
    a_valid_parent_survives_a_round_trip();
    masks_survive_a_round_trip();

    const core::Project original = makeProject();

    core::Project loaded;
    io::LoadReport report = io::fromJson(loaded, io::toJson(original));
    check(report.ok, "a project we just wrote loads back");
    check(report.clean(), "and does so without complaints");

    check(loaded.media().size() == 1, "media survives");
    check(loaded.media()[0].path == "/clips/a.mp4", "the path survives");
    check(std::fabs(loaded.media()[0].fps - 23.976) < 1e-9,
          "a non-integer frame rate survives exactly");

    check(loaded.compositions().size() == 1, "the composition survives");
    const core::Composition& comp = loaded.compositions().front();
    check(comp.width == 1080 && comp.height == 1920, "size survives");
    check(std::fabs(comp.fps - 29.97) < 1e-9, "29.97 survives exactly");

    check(comp.layers.size() == 2, "both layers survive");

    {
        const auto at = std::find_if(comp.layers.begin(), comp.layers.end(),
                                     [](const core::Layer& l) {
                                         return l.kind == core::LayerKind::Solid;
                                     });
        check(at != comp.layers.end(), "the solid survives as a solid");
        if (at != comp.layers.end()) {
            check(std::fabs(at->solidColor.c[0] - 0.25) < 1e-9 &&
                      std::fabs(at->solidColor.c[2] - 0.75) < 1e-9,
                  "its colour survives");
            check(at->solidColor.count == 4, "as four components");
            check(at->solidWidth == 640, "an explicit width survives");
            check(at->solidHeight == 0,
                  "and 0 stays 0, so it keeps following the composition");
        }
    }

    check(comp.layers.size() == 2, "the layer survives");
    const core::Layer& layer = *std::find_if(
        comp.layers.begin(), comp.layers.end(),
        [](const core::Layer& l) { return l.kind == core::LayerKind::Footage; });
    check(layer.blend == core::BlendMode::Add, "blend mode survives");
    check(layer.expanded, "twirl state survives");
    check(layer.enabled, "the eye survives");
    check(!layer.audioEnabled, "and the speaker survives independently of it");
    // A locked layer returning unlocked is worse than never locking: false security.
    check(layer.locked, "the padlock survives a save and reopen");
    // Work area is also the export range; losing it would silently change exports.
    check(comp.hasWorkArea(), "the work area survives a save and reopen");

    // Ranges aren't written to file; adoptTransformRanges restores them on load.
    const core::Property* op = layer.find("opacity");
    check(op != nullptr, "the reloaded layer still has opacity");
    if (op != nullptr) {
        check(!op->range.maximum.has_value(),
              "the file did not carry a range, and should not have");
    }
    core::adoptTransformRanges(const_cast<core::Layer&>(layer));
    const core::Property* after = layer.find("opacity");
    check(after != nullptr && after->range.maximum.has_value() &&
              *after->range.maximum == 100.0,
          "and the definition supplies it on load");
    // Group expansion is per layer/effect; reload must not force everything open.
    check(!layer.transformExpanded, "a shut Transform group stays shut");

    // Old files lack audioEnabled; must default to audible, not silently mute.
    {
        std::string json = io::toJson(original);
        const std::string key = "\"audioEnabled\": false,";
        const auto at = json.find(key);
        check(at != std::string::npos, "the field is actually written");
        json.erase(at, key.size());

        core::Project old_;
        const io::LoadReport r = io::fromJson(old_, json);
        check(r.ok, "a project without the field still loads");
        check(old_.compositions().front().layers.front().audioEnabled,
              "and its layers default to audible");
    }

    // A time authored in beats must not come back as seconds.
    check(layer.inPoint.mode == core::TimeMode::Beats, "beats stay beats");
    check(std::fabs(layer.inPoint.value - 2.0) < 1e-9, "and keep their value");
    check(layer.outPoint.mode == core::TimeMode::Seconds, "seconds stay seconds");

    check(layer.media.has_value(), "the media link survives");
    check(layer.media.has_value() &&
              ruby::core::Project(loaded).findMedia(*layer.media) != nullptr,
          "and still resolves");

    const core::Property* pos = layer.find("position");
    check(pos != nullptr, "the property survives");
    check(pos != nullptr && pos->keys.size() == 2, "both keyframes survive");
    if (pos != nullptr && pos->keys.size() == 2) {
        check(pos->keys[0].value.count == 2, "a vec2 keyframe stays a vec2");
        check(std::fabs(pos->keys[0].overshoot - 0.12) < 1e-9, "easing survives");
        check(pos->keys[1].interp == core::Interpolation::Hold,
              "hold interpolation survives");
        check(pos->keys[1].time.mode == core::TimeMode::Beats,
              "a beat-authored keyframe stays in beats");
    }
    check(pos != nullptr && pos->expression.has_value(), "an expression survives");

    check(layer.effects.size() == 1, "the effect survives");
    check(layer.effects[0].schema == 1, "its schema version is recorded for migration");
    check(layer.effects[0].params.size() == 1, "its parameters survive");

    // Hand-placed markers must survive a save too.
    check(comp.rhythm.markers().size() == 3, "every marker survives");
    check(comp.rhythm.nearestIn(5.5, {core::MarkerLane::User}).has_value(),
          "the user marker survives with its lane intact");

    // A newly added layer must not collide with an id restored from the file.
    core::Project reopened;
    check(io::fromJson(reopened, io::toJson(original)).ok, "reopen for the id check");
    core::Composition& target = reopened.compositions().front();
    const core::LayerId existing = target.layers.front().id;
    core::Layer& added = reopened.addLayer(target, "new", core::LayerKind::Solid);
    check(added.id != existing, "ids minted after a load do not collide with loaded ones");

    // --- files that are wrong ---------------------------------------------------
    core::Project untouched = makeProject();
    const std::string before = io::toJson(untouched);

    check(!io::fromJson(untouched, "{ not json").ok, "garbage is rejected");
    check(!io::fromJson(untouched, R"({"format":"something-else"})").ok,
          "another app's JSON is rejected");
    check(!io::fromJson(untouched, R"({"format":"ruby-project","schema":9999})").ok,
          "a file from a newer Ruby is refused rather than silently downgraded");
    check(io::toJson(untouched) == before,
          "a failed load leaves the existing project untouched");

    // A missing pool entry should cost the link, not the layer.
    core::Project dangling;
    const std::string broken =
        R"({"format":"ruby-project","schema":1,"media":[],"compositions":[
             {"id":1,"name":"c","width":1080,"height":1920,"fps":30,"duration":10,
              "layers":[{"id":2,"name":"orphan","kind":"footage","media":77}]}]})";
    report = io::fromJson(dangling, broken);
    check(report.ok, "a project with a dangling media reference still opens");
    check(!report.notes.empty(), "and says what it had to fix");
    check(dangling.compositions().front().layers.size() == 1, "the layer is kept");
    check(!dangling.compositions().front().layers.front().media.has_value(),
          "with the broken link dropped");

    // Sparse files are what hand-editing produces.
    core::Project sparse;
    report = io::fromJson(
        sparse, R"({"format":"ruby-project","schema":1,"compositions":[{"name":"bare"}]})");
    check(report.ok, "a file missing most fields still opens");
    check(sparse.compositions().size() == 1, "and the composition is there");
    check(sparse.compositions().front().width > 0, "with defaults filled in");

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("projectio: all checks passed");
    return EXIT_SUCCESS;
}
