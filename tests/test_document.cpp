// Tests for the document model: beat map, layers, compositions, project.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ruby/core/Document.h"

using namespace ruby::core;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void checkNear(double a, double b, const char* what, double eps = 1e-9) {
    if (std::fabs(a - b) > eps) {
        std::fprintf(stderr, "FAIL: %s (got %.8f, want %.8f)\n", what, a, b);
        ++failures;
    }
}

RhythmMap fourBars(double bpm) {
    // 16 beats at the given tempo, a downbeat every 4.
    std::vector<Marker> beats;
    const double step = 60.0 / bpm;
    for (int i = 0; i < 16; ++i) {
        Marker m;
        m.seconds = static_cast<double>(i) * step;
        m.index = i;
        m.lane = (i % 4 == 0) ? MarkerLane::Downbeat : MarkerLane::Beat;
        beats.push_back(m);
    }
    RhythmMap map;
    map.setLane(MarkerLane::Beat, {});
    for (const Marker& m : beats) {
        map.setLane(m.lane, {});
    }
    std::vector<Marker> plain, downs;
    for (const Marker& m : beats) {
        (m.lane == MarkerLane::Downbeat ? downs : plain).push_back(m);
    }
    map.setLane(MarkerLane::Beat, plain);
    map.setLane(MarkerLane::Downbeat, downs);
    map.setTempo(bpm);
    return map;
}

// Public build's map is always empty (detection is private); queries must no-op, not crash.
void an_empty_rhythm_map_is_an_ordinary_state() {
    const RhythmMap none;

    check(none.empty(), "a default rhythm map is empty");
    check(!none.nearest(3.0).has_value(), "nearest on an empty map returns nothing");
    check(!none.nearestIn(3.0, {MarkerLane::Beat}).has_value(),
          "a lane query on empty returns nothing");
    checkNear(none.snap(3.14159), 3.14159, "snapping against an empty map is a no-op");
    checkNear(none.snapTo(3.14159, {MarkerLane::Downbeat}), 3.14159,
              "lane snap on empty is a no-op");
    check(none.between(0.0, 12.0, {MarkerLane::Downbeat}).empty(),
          "no cut points without a rhythm map");
    check(!none.has(MarkerLane::Beat), "an empty map reports no lanes");
}

void markers_are_sorted_however_they_arrive() {
    RhythmMap map;
    map.setLane(MarkerLane::Vocal, {{2.0, MarkerLane::Vocal, 1.0f, 2},
                                    {0.0, MarkerLane::Vocal, 1.0f, 0},
                                    {1.0, MarkerLane::Vocal, 1.0f, 1}});
    check(map.markers().size() == 3, "all markers retained");
    checkNear(map.markers()[0].seconds, 0.0, "sorted first");
    checkNear(map.markers()[1].seconds, 1.0, "sorted second");
    checkNear(map.markers()[2].seconds, 2.0, "sorted third");
}

void snapping_picks_the_nearest_marker() {
    const RhythmMap map = fourBars(120.0);  // a marker every 0.5s

    checkNear(map.snap(0.6), 0.5, "0.6s snaps back to 0.5");
    checkNear(map.snap(0.9), 1.0, "0.9s snaps forward to 1.0");
    checkNear(map.snap(0.0), 0.0, "an exact marker stays put");
}

// Downbeat query must ignore ordinary beats in between.
void downbeat_snapping_ignores_ordinary_beats() {
    const RhythmMap map = fourBars(120.0);  // downbeats at 0, 2, 4, 6s

    checkNear(map.snapTo(0.6, {MarkerLane::Downbeat}), 0.0, "0.6s snaps to the downbeat at 0");
    checkNear(map.snapTo(1.7, {MarkerLane::Downbeat}), 2.0, "1.7s snaps to the downbeat at 2");
    checkNear(map.snap(1.7), 1.5, "plain snap would have gone to 1.5 instead");
}

void cut_points_are_a_half_open_range() {
    const RhythmMap map = fourBars(120.0);  // downbeats at 0, 2, 4, 6
    const std::vector<double> cuts = map.between(0.0, 4.0, {MarkerLane::Downbeat});

    check(cuts.size() == 2, "0..4 contains two downbeats");
    checkNear(cuts[0], 0.0, "range includes its start");
    checkNear(cuts[1], 2.0, "second cut point");
    check(map.between(0.0, 0.0, {MarkerLane::Downbeat}).empty(),
          "an empty range yields no cuts");
}

// Re-running detection must never discard a correction someone made by hand.
void re_analysis_preserves_user_markers() {
    RhythmMap map = fourBars(120.0);
    map.addUserMarker(1.23);

    map.setLane(MarkerLane::Beat, {{9.0, MarkerLane::Beat, 1.0f, 0}});
    map.setLane(MarkerLane::Downbeat, {});

    const auto user = map.nearestIn(1.23, {MarkerLane::User});
    check(user.has_value(), "the hand-placed marker survived re-analysis");
    check(user.has_value() && std::fabs(user->seconds - 1.23) < 1e-9,
          "and it is exactly where it was put");
    check(!map.has(MarkerLane::Downbeat), "the replaced lane really was replaced");

    check(map.removeMarkerNear(1.23, 0.05), "a user marker can be removed");
    check(!map.nearestIn(1.23, {MarkerLane::User}).has_value(), "and then it is gone");
}

// Vocal onsets are aperiodic; a vocal-only map must not claim a tempo.
void a_vocal_only_map_supplies_no_tempo() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);
    comp.rhythm.setLane(MarkerLane::Vocal,
                        {{0.4, MarkerLane::Vocal, 1.0f, 0},
                         {1.1, MarkerLane::Vocal, 1.0f, 1}});

    const TimeContext ctx = comp.timeContext();
    check(!ctx.has_beat_map, "syllables are not a grid, so there is no beat map");
    checkNear(ctx.bpm, 120.0, "and the nominal fallback tempo still applies");
    check(!comp.rhythm.empty(), "even though the map has markers in it");
}

void layer_labels_follow_the_design() {
    check(defaultLabelFor(LayerKind::Text) == LabelColor::Lavender, "text is lavender");
    check(defaultLabelFor(LayerKind::Shape) == LabelColor::Lavender, "shape is lavender");
    check(defaultLabelFor(LayerKind::Precomp) == LabelColor::Aqua, "precomp is aqua");
    check(defaultLabelFor(LayerKind::Audio) == LabelColor::Green, "audio is green");
    check(defaultLabelFor(LayerKind::Footage) == LabelColor::Gray, "footage is gray");
}

// Pixel-stored position breaks AE presets when the comp is reshaped; must default to a resolution-independent unit.
void the_default_transform_stores_resolution_independent_units() {
    const std::vector<Property> t = defaultTransform();
    check(t.size() == 5, "five transform properties");

    const auto findKey = [&t](const char* key) -> const Property* {
        for (const Property& p : t) {
            if (p.key == key) {
                return &p;
            }
        }
        return nullptr;
    };

    const Property* pos = findKey("position");
    check(pos != nullptr, "position exists");
    check(pos != nullptr && pos->unit != SpatialUnit::Px,
          "position is not stored in pixels");

    const Property* rot = findKey("rotation");
    check(rot != nullptr && rot->unit == SpatialUnit::Degrees, "rotation is in degrees");

    const Property* op = findKey("opacity");
    check(op != nullptr && op->staticValue.x() == 100.0, "opacity defaults to 100");
}

void new_layers_land_on_top() {
    Project project;
    Composition& comp = project.addComposition("sneaker_drop", 1080, 1920, 30.0, 12.0);

    project.addLayer(comp, "footage", LayerKind::Footage);
    project.addLayer(comp, "captions", LayerKind::Precomp);
    Layer& newest = project.addLayer(comp, "DROP 09.12", LayerKind::Text);

    check(comp.layers.size() == 3, "three layers");
    check(comp.layers.front().name == "DROP 09.12", "the newest layer is topmost, as in AE");
    check(newest.label == LabelColor::Lavender, "a text layer gets the lavender label");
    check(newest.find("position") != nullptr, "a new layer has its transform stack");
    check(newest.find("nonexistent") == nullptr, "looking up an unknown key returns null");
}

void keyframe_counts_roll_up() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);
    Layer& layer = project.addLayer(comp, "text", LayerKind::Text);

    const TimeContext ctx = comp.timeContext();
    Property* pos = layer.find("position");
    check(pos != nullptr, "position found");
    if (pos != nullptr) {
        pos->addKey({TimeValue::seconds(0.6), Value::vec2(0, 0), Interpolation::Bezier, 0, 0, 0},
                    ctx);
        pos->addKey({TimeValue::seconds(1.3), Value::vec2(1, 1), Interpolation::Bezier, 0, 0, 0},
                    ctx);
    }

    check(comp.layers.front().keyframeCount() == 2, "layer counts its keyframes");
    check(comp.totalKeyframes() == 2, "composition rolls up layer keyframes");
}

// In the public build there is no detector, so `beats` mode still has to resolve.
void time_context_falls_back_without_a_beat_map() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);

    TimeContext ctx = comp.timeContext();
    check(!ctx.has_beat_map, "a fresh comp has no rhythm map");
    checkNear(ctx.bpm, 120.0, "falls back to a nominal tempo so beats still resolve");
    checkNear(to_seconds(TimeValue::beats(4.0), ctx), 2.0, "one bar at the fallback tempo");

    comp.rhythm = fourBars(174.0);
    ctx = comp.timeContext();
    check(ctx.has_beat_map, "a beat lane with a tempo makes the map present");
    checkNear(ctx.bpm, 174.0, "tempo now comes from the map");
}

// A composition grows to hold what is in it, and never pulls back in on its own.
void a_composition_grows_to_hold_its_content() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);

    check(!comp.growToFit(), "an empty comp has nothing to grow around");
    checkNear(comp.duration, 12.0, "and its duration is left alone");
    checkNear(comp.contentEnd(), 0.0, "an empty comp ends at zero");

    Layer& shortClip = project.addLayer(comp, "short", LayerKind::Footage);
    shortClip.inPoint = TimeValue::seconds(0.0);
    shortClip.outPoint = TimeValue::seconds(5.0);
    check(!comp.growToFit(), "a layer inside the comp does not move the end");
    checkNear(comp.duration, 12.0, "duration unchanged");

    Layer& longClip = project.addLayer(comp, "long", LayerKind::Footage);
    longClip.inPoint = TimeValue::seconds(8.6);
    longClip.outPoint = TimeValue::seconds(18.4);
    checkNear(comp.contentEnd(), 18.4, "the end is the last layer's out point");
    check(comp.growToFit(), "a layer past the end grows the comp");
    checkNear(comp.duration, 18.4, "grown to exactly the content end, with no padding");

    check(!comp.growToFit(), "growing is idempotent");
}

// The whole point of the rule: growth is automatic, shrinking is the user's job.
void a_composition_never_shrinks_itself() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);

    Layer& clip = project.addLayer(comp, "long", LayerKind::Footage);
    clip.inPoint = TimeValue::seconds(0.0);
    clip.outPoint = TimeValue::seconds(60.0);
    check(comp.growToFit(), "grew to fit the long clip");
    checkNear(comp.duration, 60.0, "sixty seconds");

    // Trimming leaves the tail on purpose; shrinking is a manual Composition Settings action.
    clip.outPoint = TimeValue::seconds(2.0);
    check(!comp.growToFit(), "a shorter layer does not shrink the comp");
    checkNear(comp.duration, 60.0, "the empty tail stays until the user removes it");

    comp.layers.clear();
    check(!comp.growToFit(), "an emptied comp does not collapse");
    checkNear(comp.duration, 60.0, "still sixty seconds");
}

// Manual shrink must not truncate overhanging layers; that's the clamp this design avoids.
void a_manual_shrink_leaves_layers_overhanging() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 60.0);

    Layer& clip = project.addLayer(comp, "clip", LayerKind::Footage);
    clip.inPoint = TimeValue::seconds(0.0);
    clip.outPoint = TimeValue::seconds(40.0);

    comp.duration = 10.0;  // what Composition Settings will do
    checkNear(comp.contentEnd(), 40.0, "the layer keeps its full extent");
    check(comp.growToFit(), "and asking to fit again grows it straight back");
    checkNear(comp.duration, 40.0, "no footage was lost by shrinking");
}

// Beats-timed layers must resolve before comparing to a seconds duration.
void growth_resolves_layers_timed_in_beats() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 4.0);
    comp.rhythm = fourBars(120.0);

    Layer& clip = project.addLayer(comp, "clip", LayerKind::Footage);
    clip.inPoint = TimeValue::beats(0.0);
    clip.outPoint = TimeValue::beats(16.0);  // 8s at 120bpm

    checkNear(comp.contentEnd(), 8.0, "beats resolved against the comp's own tempo");
    check(comp.growToFit(), "grew past its four seconds");
    checkNear(comp.duration, 8.0, "eight seconds");
}


// Deleting a layer must not leave a parent link pointing into a hole.
void removing_a_layer_orphans_its_children() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);

    // addLayer inserts at front, so grab the id now; the earlier reference won't survive.
    const LayerId parentId = project.addLayer(comp, "null", LayerKind::Null).id;
    project.addLayer(comp, "text", LayerKind::Text).parent = parentId;

    check(!comp.removeLayer(9999), "removing an id that is not here reports failure");
    check(comp.layers.size() == 2, "and removes nothing");

    check(comp.removeLayer(parentId), "the parent is removed");
    check(comp.layers.size() == 1, "one layer left");

    const Layer* orphan = comp.find(comp.layers.front().id);
    check(orphan != nullptr, "the child survived");
    check(!orphan->parent.has_value(),
          "and its parent link was cleared rather than left dangling");
}


void created_layers_have_sane_defaults() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);

    const LayerId solidId = project.addLayer(comp, "Backdrop", LayerKind::Solid).id;
    const Layer* solid = comp.find(solidId);
    check(solid != nullptr, "the solid was created");
    check(solid->kind == LayerKind::Solid, "as a solid");
    check(solid->solidWidth == 0 && solid->solidHeight == 0,
          "sized 0 by default, meaning it follows the composition rather than freezing "
          "at today's size");
    check(solid->solidColor.count == 4, "its colour is four components");
    check(!solid->properties.empty(),
          "and it gets the standard transform, so it can be moved and scaled");

    const LayerId nullId = project.addLayer(comp, "Null", LayerKind::Null).id;
    const Layer* nul = comp.find(nullId);
    check(nul != nullptr, "the null was created");
    check(!nul->properties.empty(),
          "a null is a transform with a handle, so it needs the transform most of all");
}

// Nulls exist to be parented to, so this is the case that actually happens.
void deleting_a_null_releases_what_it_drove() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);

    const LayerId nullId = project.addLayer(comp, "Null", LayerKind::Null).id;
    project.addLayer(comp, "Backdrop", LayerKind::Solid).parent = nullId;
    project.addLayer(comp, "clip", LayerKind::Footage).parent = nullId;

    check(comp.removeLayer(nullId), "the null is removed");
    check(comp.layers.size() == 2, "its children are not");
    for (const Layer& l : comp.layers) {
        check(!l.parent.has_value(), "and none of them still point at it");
    }
}

// Removing a clip from the project must not take the edit with it.
void removing_media_clears_the_layers_that_used_it() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);
    const MediaId clip =
        project.addMedia("/a/clip.mov", "clip.mov", MediaKind::Video, 5.0, 1920, 1080,
                         30.0, true)
            .id;
    const MediaId other =
        project.addMedia("/a/song.wav", "song.wav", MediaKind::Audio, 60.0, 0, 0, 0.0,
                         true)
            .id;

    project.addLayer(comp, "one", LayerKind::Footage).media = clip;
    project.addLayer(comp, "two", LayerKind::Footage).media = clip;
    project.addLayer(comp, "song", LayerKind::Audio).media = other;

    check(project.usageCount(clip) == 2, "two layers use the clip");
    check(project.usageCount(other) == 1, "one uses the song");

    check(project.removeMedia(clip) == 2, "removing it reports how many were affected");
    check(project.media().size() == 1, "the item is gone from the pool");
    check(comp.layers.size() == 3,
          "but the layers stay: removing a clip is not deleting an edit");

    for (const Layer& l : comp.layers) {
        if (l.name == "one" || l.name == "two") {
            check(!l.media.has_value(),
                  "layers that used it have no media rather than a dangling id");
        }
    }
    check(project.usageCount(other) == 1, "an unrelated layer keeps its media");

    check(project.removeMedia(9999) == 0, "removing something absent affects nothing");
    check(project.media().size() == 1, "and removes nothing");
}

// "No work area" and "work area covering everything" must behave identically.
void the_work_area_defaults_to_the_whole_composition() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);

    check(!comp.hasWorkArea(), "a fresh composition has no work area");
    double from = -1.0;
    double to = -1.0;
    comp.workRange(from, to);
    check(from == 0.0 && to == 12.0, "and reports the whole thing anyway");

    comp.workIn = TimeValue::seconds(2.0);
    comp.workOut = TimeValue::seconds(5.0);
    check(comp.hasWorkArea(), "once set, it is set");
    comp.workRange(from, to);
    check(from == 2.0 && to == 5.0, "and reports itself");
}

// A range of no length is not a smaller selection, it is no selection.
void a_work_area_of_no_length_is_no_work_area() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);
    comp.workIn = TimeValue::seconds(4.0);
    comp.workOut = TimeValue::seconds(4.0);
    check(!comp.hasWorkArea(), "zero length does not count");

    double from = 0.0;
    double to = 0.0;
    comp.workRange(from, to);
    check(from == 0.0 && to == 12.0, "so the whole composition is live again");
}

// A work area past the end would have the cache filling nonexistent frames.
void the_work_area_is_clamped_to_the_composition() {
    Project project;
    Composition& comp = project.addComposition("c", 1080, 1920, 30.0, 12.0);
    comp.workIn = TimeValue::seconds(-5.0);
    comp.workOut = TimeValue::seconds(900.0);

    double from = 0.0;
    double to = 0.0;
    comp.workRange(from, to);
    check(from == 0.0, "the start cannot be negative");
    check(to == 12.0, "and the end cannot be past the duration");
}

}  // namespace

int main() {
    an_empty_rhythm_map_is_an_ordinary_state();
    markers_are_sorted_however_they_arrive();
    snapping_picks_the_nearest_marker();
    downbeat_snapping_ignores_ordinary_beats();
    cut_points_are_a_half_open_range();
    re_analysis_preserves_user_markers();
    a_vocal_only_map_supplies_no_tempo();
    layer_labels_follow_the_design();
    the_default_transform_stores_resolution_independent_units();
    new_layers_land_on_top();
    keyframe_counts_roll_up();
    time_context_falls_back_without_a_beat_map();
    the_work_area_defaults_to_the_whole_composition();
    a_work_area_of_no_length_is_no_work_area();
    the_work_area_is_clamped_to_the_composition();
    a_composition_grows_to_hold_its_content();
    a_composition_never_shrinks_itself();
    a_manual_shrink_leaves_layers_overhanging();
    growth_resolves_layers_timed_in_beats();
    removing_a_layer_orphans_its_children();
    created_layers_have_sane_defaults();
    deleting_a_null_releases_what_it_drove();
    removing_media_clears_the_layers_that_used_it();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("document: all checks passed");
    return EXIT_SUCCESS;
}
