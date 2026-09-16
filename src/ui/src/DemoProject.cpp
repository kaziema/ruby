#include "ruby/ui/DemoProject.h"

#include "ruby/engine/EffectRegistry.h"
#include "ruby/media/Probe.h"

#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace ruby::ui::demo {

using namespace core;

namespace {

// Writes keys from `from` to `to`. Values must match the property's shape (e.g. vec2),
// or it collapses to a scalar once animated.
void animate(Property& p, const TimeContext& ctx, std::initializer_list<double> times,
             const Value& from, const Value& to) {
    p.staticValue = from;

    const std::vector<double> at(times);
    const auto count = static_cast<double>(at.size());

    for (std::size_t i = 0; i < at.size(); ++i) {
        const double t = (at.size() < 2) ? 1.0 : static_cast<double>(i) / (count - 1.0);

        Keyframe k;
        k.time = TimeValue::seconds(at[i]);
        k.value = lerp(from, to, t);
        k.interp = Interpolation::Bezier;
        k.easeOut = 0.68;
        k.easeIn = 0.68;
        p.addKey(k, ctx);
    }
}

Property makeProp(std::string key, std::string label, std::string group, SpatialUnit unit,
                  Value initial) {
    Property p;
    p.key = std::move(key);
    p.label = std::move(label);
    p.group = std::move(group);
    p.unit = unit;
    p.staticValue = initial;
    return p;
}

}  // namespace

namespace {

// TEMPORARY: paths come from the command line until the Project panel can import media.
std::vector<std::string> g_mediaPaths;

// Imports a path into the media pool and returns its id.
std::optional<MediaId> importAt(Project& project, std::size_t index) {
    if (index >= g_mediaPaths.size() || g_mediaPaths[index].empty()) {
        return std::nullopt;
    }
    const std::string& path = g_mediaPaths[index];
    const auto info = media::probe(path);
    if (!info.has_value()) {
        return std::nullopt;
    }

    const auto slash = path.find_last_of('/');
    const std::string name =
        (slash == std::string::npos) ? path : path.substr(slash + 1);

    const MediaKind kind = info->hasVideo ? MediaKind::Video : MediaKind::Audio;
    return project.addMedia(path, name, kind, info->duration, info->width, info->height,
                            info->fps, info->hasAudio)
        .id;
}

}  // namespace

void setMediaPaths(std::vector<std::string> paths) { g_mediaPaths = std::move(paths); }

Project sampleProject() {
    Project project;
    Composition& comp = project.addComposition("sneaker_drop_v4", 1080, 1920, 30.0, 12.0);

    const TimeContext ctx = comp.timeContext();

    // Added bottom-up, because addLayer puts each new layer on top.
    Layer& audio = project.addLayer(comp, "track_hardwave.wav", LayerKind::Audio);
    audio.inPoint = TimeValue::seconds(0.0);
    audio.outPoint = TimeValue::seconds(12.0);
    // Any supplied file will do as a track; we only want its audio stream.
    audio.media = importAt(project, 2).has_value() ? importAt(project, 2)
                                                   : importAt(project, 0);

    Layer& broll = project.addLayer(comp, "b-roll_street.mp4", LayerKind::Footage);
    broll.inPoint = TimeValue::seconds(6.8);
    broll.outPoint = TimeValue::seconds(12.0);
    broll.media = importAt(project, 1);

    Layer& sneaker = project.addLayer(comp, "sneaker_a4.mp4", LayerKind::Footage);
    sneaker.inPoint = TimeValue::seconds(0.0);
    sneaker.outPoint = TimeValue::seconds(7.2);
    sneaker.media = importAt(project, 0);
    sneaker.expanded = true;
    {
        // Animated grade, so the effect is visibly doing something rather than static.
        EffectInstance grade = engine::EffectRegistry::instance().instantiate(
            "core.color.grade");
        if (Property* saturation = grade.find("saturation"); saturation != nullptr) {
            animate(*saturation, ctx, {0.0, 3.5, 7.0}, Value::scalar(0.0),
                    Value::scalar(160.0));
        }
        if (Property* contrast = grade.find("contrast"); contrast != nullptr) {
            contrast->staticValue = Value::scalar(118.0);
        }
        sneaker.effects.push_back(std::move(grade));
    }

    Layer& captions = project.addLayer(comp, "captions", LayerKind::Precomp);
    captions.inPoint = TimeValue::seconds(1.2);
    captions.outPoint = TimeValue::seconds(10.6);
    captions.expanded = true;
    // Empty precomp fills the frame; drop opacity so footage underneath shows through.
    if (Property* op = captions.find("opacity"); op != nullptr) {
        op->staticValue = Value::scalar(35.0);
    }
    {
        Property hits = makeProp("word_pop", "word_pop", "Source", SpatialUnit::Normalized,
                                 Value::scalar(0.0));
        animate(hits, ctx, {1.4, 2.3, 3.1, 4.2, 5.4, 6.6, 8.0, 9.4}, Value::scalar(1.0),
                Value::scalar(8.0));
        captions.properties.push_back(std::move(hits));
    }

    Layer& wipe = project.addLayer(comp, "swipe wipe", LayerKind::Shape);
    wipe.inPoint = TimeValue::seconds(3.9);
    wipe.outPoint = TimeValue::seconds(5.3);
    wipe.blend = BlendMode::Add;
    if (Property* pos = wipe.find("position"); pos != nullptr) {
        animate(*pos, ctx, {4.0, 4.6, 5.2}, Value::vec2(-20.0, 50.0),
                Value::vec2(120.0, 50.0));
    }

    Layer& title = project.addLayer(comp, "DROP 09.12", LayerKind::Text);
    title.inPoint = TimeValue::seconds(0.4);
    title.outPoint = TimeValue::seconds(5.0);
    title.expanded = true;
    {
        // Position is a frame percentage, not pixels.
        if (Property* pos = title.find("position"); pos != nullptr) {
            animate(*pos, ctx, {0.6, 1.3, 2.66, 4.4}, Value::vec2(50.0, 88.0),
                    Value::vec2(50.0, 42.0));
        }
        if (Property* scale = title.find("scale"); scale != nullptr) {
            animate(*scale, ctx, {0.6, 1.1, 2.66}, Value::vec2(0.0, 0.0),
                    Value::vec2(100.0, 100.0));
        }
        Property tracking = makeProp("tracking", "Tracking", "Text", SpatialUnit::Normalized,
                                     Value::scalar(0.0));
        animate(tracking, ctx, {0.6, 2.0}, Value::scalar(0.0), Value::scalar(12.0));
        title.properties.push_back(std::move(tracking));
    }

    return project;
}

}  // namespace ruby::ui::demo
