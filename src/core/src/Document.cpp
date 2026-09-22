#include "ruby/core/Document.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ruby::core {

// --- RhythmMap ---------------------------------------------------------------

void RhythmMap::resort() {
    std::sort(markers_.begin(), markers_.end(),
              [](const Marker& a, const Marker& b) { return a.seconds < b.seconds; });
}

void RhythmMap::setLane(MarkerLane lane, std::vector<Marker> markers) {
    // User markers survive re-analysis, so correcting the detector isn't wasted on rerun.
    std::erase_if(markers_, [lane](const Marker& m) { return m.lane == lane; });
    for (Marker& m : markers) {
        m.lane = lane;
    }
    markers_.insert(markers_.end(), markers.begin(), markers.end());
    resort();
}

void RhythmMap::clearLane(MarkerLane lane) {
    std::erase_if(markers_, [lane](const Marker& m) { return m.lane == lane; });
}

void RhythmMap::addUserMarker(double seconds) {
    Marker m;
    m.seconds = seconds;
    m.lane = MarkerLane::User;
    m.strength = 1.0f;
    markers_.push_back(m);
    resort();
}

bool RhythmMap::removeMarkerNear(double seconds, double tolerance) {
    const auto it = std::find_if(markers_.begin(), markers_.end(), [&](const Marker& m) {
        return std::fabs(m.seconds - seconds) <= tolerance;
    });
    if (it == markers_.end()) {
        return false;
    }
    markers_.erase(it);
    return true;
}

bool RhythmMap::has(MarkerLane lane) const noexcept {
    return std::any_of(markers_.begin(), markers_.end(),
                       [lane](const Marker& m) { return m.lane == lane; });
}

namespace {

bool inLanes(MarkerLane lane, std::initializer_list<MarkerLane> lanes) {
    return std::find(lanes.begin(), lanes.end(), lane) != lanes.end();
}

}  // namespace

std::optional<Marker> RhythmMap::nearest(double seconds) const {
    const Marker* best = nullptr;
    double bestDist = 0.0;
    for (const Marker& m : markers_) {
        const double d = std::fabs(m.seconds - seconds);
        if (best == nullptr || d < bestDist) {
            best = &m;
            bestDist = d;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<Marker>(*best);
}

std::optional<Marker> RhythmMap::nearestIn(double seconds,
                                           std::initializer_list<MarkerLane> lanes) const {
    const Marker* best = nullptr;
    double bestDist = 0.0;
    for (const Marker& m : markers_) {
        if (!inLanes(m.lane, lanes)) {
            continue;
        }
        const double d = std::fabs(m.seconds - seconds);
        if (best == nullptr || d < bestDist) {
            best = &m;
            bestDist = d;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<Marker>(*best);
}

double RhythmMap::snap(double seconds) const {
    const auto m = nearest(seconds);
    return m ? m->seconds : seconds;
}

double RhythmMap::snapTo(double seconds, std::initializer_list<MarkerLane> lanes) const {
    const auto m = nearestIn(seconds, lanes);
    return m ? m->seconds : seconds;
}

std::vector<double> RhythmMap::between(double from, double to,
                                       std::initializer_list<MarkerLane> lanes) const {
    std::vector<double> out;
    for (const Marker& m : markers_) {
        if (inLanes(m.lane, lanes) && m.seconds >= from && m.seconds < to) {
            out.push_back(m.seconds);
        }
    }
    return out;
}

// --- Layer -------------------------------------------------------------------

LabelColor defaultLabelFor(LayerKind kind) noexcept {
    switch (kind) {
        case LayerKind::Text:
        case LayerKind::Shape:
            return LabelColor::Lavender;
        case LayerKind::Precomp:
            return LabelColor::Aqua;
        case LayerKind::Audio:
            return LabelColor::Green;
        case LayerKind::Footage:
        case LayerKind::Solid:
        case LayerKind::Adjustment:
        case LayerKind::Null:
            return LabelColor::Gray;
    }
    return LabelColor::Gray;
}

Property* EffectInstance::find(std::string_view key) noexcept {
    const auto it = std::find_if(params.begin(), params.end(),
                                 [key](const Property& p) { return p.key == key; });
    return it == params.end() ? nullptr : &*it;
}

const Property* EffectInstance::find(std::string_view key) const noexcept {
    const auto it = std::find_if(params.begin(), params.end(),
                                 [key](const Property& p) { return p.key == key; });
    return it == params.end() ? nullptr : &*it;
}

std::size_t EffectInstance::propertyCount() const noexcept {
    std::size_t n = params.size();
    for (const Mask& m : masks) {
        n += m.params.size();
    }
    return n;
}

const Property* EffectInstance::property(std::size_t index) const noexcept {
    if (index < params.size()) {
        return &params[index];
    }
    index -= params.size();
    for (const Mask& m : masks) {
        if (index < m.params.size()) {
            return &m.params[index];
        }
        index -= m.params.size();
    }
    return nullptr;
}

Property* EffectInstance::property(std::size_t index) noexcept {
    return const_cast<Property*>(std::as_const(*this).property(index));
}

int EffectInstance::maskOf(std::size_t index) const noexcept {
    if (index < params.size()) {
        return -1;
    }
    index -= params.size();
    for (std::size_t m = 0; m < masks.size(); ++m) {
        if (index < masks[m].params.size()) {
            return static_cast<int>(m);
        }
        index -= masks[m].params.size();
    }
    return -1;
}

Property* Mask::find(std::string_view key) noexcept {
    const auto it = std::find_if(params.begin(), params.end(),
                                 [key](const Property& p) { return p.key == key; });
    return it == params.end() ? nullptr : &*it;
}

const Property* Mask::find(std::string_view key) const noexcept {
    const auto it = std::find_if(params.begin(), params.end(),
                                 [key](const Property& p) { return p.key == key; });
    return it == params.end() ? nullptr : &*it;
}

Mask makeMask(MaskShape shape, std::string name) {
    Mask mask;
    mask.name = std::move(name);
    mask.shape = shape;

    const auto make = [&mask](const char* key, const char* label, SpatialUnit unit,
                              ParamRange range, Value value) {
        Property p;
        p.key = key;
        p.label = mask.name + " " + label;
        p.group = mask.name;
        p.unit = unit;
        p.range = range;
        p.staticValue = value;
        mask.params.push_back(std::move(p));
    };
    make("center", "Center", SpatialUnit::Percent, ParamRange::unbounded(-50.0, 150.0),
         Value::vec2(50.0, 50.0));
    make("size", "Size", SpatialUnit::Percent, ParamRange::atLeast(0.0, 200.0),
         Value::vec2(50.0, 50.0));
    make("rotation", "Rotation", SpatialUnit::Degrees, ParamRange::unbounded(-360.0, 360.0),
         Value::scalar(0.0));
    make("feather", "Feather", SpatialUnit::PercentOfWidth, ParamRange::atLeast(0.0, 25.0),
         Value::scalar(0.0));
    // Negative shrinks the shape; a hard floor would forbid a real edit.
    make("expansion", "Expansion", SpatialUnit::PercentOfWidth,
         ParamRange::unbounded(-25.0, 25.0), Value::scalar(0.0));
    make("opacity", "Opacity", SpatialUnit::Percent, ParamRange::between(0.0, 100.0),
         Value::scalar(100.0));
    return mask;
}

std::vector<const Mask*> activeMasks(const EffectInstance& effect) {
    std::vector<const Mask*> out;
    for (const Mask& m : effect.masks) {
        if (m.mode == MaskMode::None) {
            continue;
        }
        out.push_back(&m);
        if (static_cast<int>(out.size()) == EffectInstance::kMaxMasks) {
            break;
        }
    }
    return out;
}

Property* Layer::find(std::string_view key) noexcept {
    const auto it = std::find_if(properties.begin(), properties.end(),
                                 [key](const Property& p) { return p.key == key; });
    return it == properties.end() ? nullptr : &*it;
}

const Property* Layer::find(std::string_view key) const noexcept {
    const auto it = std::find_if(properties.begin(), properties.end(),
                                 [key](const Property& p) { return p.key == key; });
    return it == properties.end() ? nullptr : &*it;
}

int Layer::keyframeCount() const noexcept {
    int total = 0;
    for (const Property& p : properties) {
        total += static_cast<int>(p.keys.size());
    }
    for (const EffectInstance& effect : effects) {
        for (std::size_t i = 0; i < effect.propertyCount(); ++i) {
            total += static_cast<int>(effect.property(i)->keys.size());
        }
    }
    return total;
}

std::vector<Property> defaultTransform() {
    // Position/anchor are stored as a fraction of the frame, so a preset lands
    // correctly across aspect ratios.
    std::vector<Property> t;

    // Four of five are deliberately unbounded — clamping would mean the app deciding
    // what an edit can look like. Only Opacity has a real limit (0-100%).

    Property anchor;
    anchor.key = "anchor_point";
    anchor.label = "Anchor Point";
    anchor.unit = SpatialUnit::PercentOfWidth;
    anchor.range = ParamRange::unbounded(-100.0, 100.0);
    anchor.staticValue = Value::vec2(0.0, 0.0);
    t.push_back(anchor);

    Property position;
    position.key = "position";
    position.label = "Position";
    position.unit = SpatialUnit::PercentOfWidth;
    // Slider extends a frame beyond the edges — moving layers in from off-screen is common.
    position.range = ParamRange::unbounded(-100.0, 200.0);
    position.staticValue = Value::vec2(50.0, 50.0);
    t.push_back(position);

    Property scale;
    scale.key = "scale";
    scale.label = "Scale";
    scale.unit = SpatialUnit::Percent;
    // Negative is a mirror, not an error.
    scale.range = ParamRange::unbounded(-200.0, 400.0);
    scale.staticValue = Value::vec2(100.0, 100.0);
    t.push_back(scale);

    Property rotation;
    rotation.key = "rotation";
    rotation.label = "Rotation";
    rotation.unit = SpatialUnit::Degrees;
    rotation.range = ParamRange::unbounded(-360.0, 360.0);
    rotation.staticValue = Value::scalar(0.0);
    t.push_back(rotation);

    Property opacity;
    opacity.key = "opacity";
    opacity.label = "Opacity";
    opacity.unit = SpatialUnit::Percent;
    opacity.range = ParamRange::between(0.0, 100.0);
    opacity.staticValue = Value::scalar(100.0);
    t.push_back(opacity);

    return t;
}

void adoptTransformRanges(Layer& layer) {
    static const std::vector<Property> canonical = defaultTransform();
    for (Property& p : layer.properties) {
        const auto it = std::find_if(canonical.begin(), canonical.end(),
                                     [&p](const Property& c) { return c.key == p.key; });
        if (it != canonical.end()) {
            p.range = it->range;
        }
    }
}

void ensureAudioLevel(Layer& layer) {
    if (layer.find(kAudioLevelKey) != nullptr) {
        return;
    }
    Property level;
    level.key = kAudioLevelKey;
    level.label = "Audio Level";
    level.group = "Audio";
    level.unit = SpatialUnit::Decibels;
    // Hard floor at true silence; no hard ceiling, soft slider to +12dB.
    level.range = ParamRange::atLeast(0.0, decibelsToLinear(12.0));
    level.staticValue = Value::scalar(1.0);
    layer.properties.push_back(level);
}

// --- Composition -------------------------------------------------------------

bool Composition::hasWorkArea() const noexcept {
    const TimeContext ctx = timeContext();
    return to_seconds(workOut, ctx) > to_seconds(workIn, ctx);
}

void Composition::workRange(double& startSeconds, double& endSeconds) const noexcept {
    const TimeContext ctx = timeContext();
    if (hasWorkArea()) {
        startSeconds = std::max(0.0, to_seconds(workIn, ctx));
        endSeconds = std::min(duration, to_seconds(workOut, ctx));
        return;
    }
    startSeconds = 0.0;
    endSeconds = duration;
}

TimeContext Composition::timeContext() const noexcept {
    TimeContext ctx;
    ctx.fps = fps;
    // `beats` mode needs a beat lane with tempo; a vocal-only map has markers but no grid.
    ctx.has_beat_map = rhythm.has(MarkerLane::Beat) && rhythm.bpm() > 0.0;
    // Nominal fallback tempo so `beats` mode still resolves when the detector is absent.
    ctx.bpm = ctx.has_beat_map ? rhythm.bpm() : 120.0;
    return ctx;
}

Layer* Composition::find(LayerId layer) noexcept {
    const auto it = std::find_if(layers.begin(), layers.end(),
                                 [layer](const Layer& l) { return l.id == layer; });
    return it == layers.end() ? nullptr : &*it;
}

const Layer* Composition::find(LayerId layer) const noexcept {
    const auto it = std::find_if(layers.begin(), layers.end(),
                                 [layer](const Layer& l) { return l.id == layer; });
    return it == layers.end() ? nullptr : &*it;
}

LayerId Composition::nextLayerId() const noexcept {
    LayerId highest = 0;
    for (const Layer& l : layers) {
        highest = std::max(highest, l.id);
    }
    return highest + 1;
}

int Composition::totalKeyframes() const noexcept {
    int total = 0;
    for (const Layer& l : layers) {
        total += l.keyframeCount();
    }
    return total;
}

std::size_t Project::usageCount(MediaId media) const noexcept {
    std::size_t used = 0;
    for (const Composition& comp : comps_) {
        for (const Layer& layer : comp.layers) {
            if (layer.media.has_value() && *layer.media == media) {
                ++used;
            }
        }
    }
    return used;
}

std::size_t Project::removeMedia(MediaId media) {
    const std::size_t affected = usageCount(media);

    // Layers stay; only the media link goes — removing a clip shouldn't delete the edit.
    // An unlinked layer draws a placeholder.
    for (Composition& comp : comps_) {
        for (Layer& layer : comp.layers) {
            if (layer.media.has_value() && *layer.media == media) {
                layer.media.reset();
            }
        }
    }
    media_.erase(std::remove_if(media_.begin(), media_.end(),
                                [media](const MediaItem& item) { return item.id == media; }),
                 media_.end());
    return affected;
}

bool Composition::removeLayer(LayerId layer) noexcept {
    const auto at = std::find_if(layers.begin(), layers.end(),
                                 [layer](const Layer& l) { return l.id == layer; });
    if (at == layers.end()) {
        return false;
    }
    layers.erase(at);
    for (Layer& l : layers) {
        if (l.parent.has_value() && *l.parent == layer) {
            l.parent.reset();
        }
    }
    return true;
}

double Composition::contentEnd() const noexcept {
    const TimeContext ctx = timeContext();
    double end = 0.0;
    for (const Layer& l : layers) {
        end = std::max(end, to_seconds(l.outPoint, ctx));
    }
    return end;
}

bool Composition::growToFit() noexcept {
    const double end = contentEnd();
    if (end <= duration) {
        return false;
    }
    duration = end;
    return true;
}

// --- Project -----------------------------------------------------------------

Composition& Project::addComposition(std::string name, int w, int h,
                                     double framesPerSecond, double durationSeconds) {
    Composition comp;
    comp.id = nextId_++;
    comp.name = std::move(name);
    comp.width = w;
    comp.height = h;
    comp.fps = framesPerSecond;
    comp.duration = durationSeconds;
    comps_.push_back(std::move(comp));
    return comps_.back();
}

Layer& Project::addLayer(Composition& comp, std::string name, LayerKind kind) {
    Layer layer;
    layer.id = nextId_++;
    layer.name = std::move(name);
    layer.kind = kind;
    layer.label = defaultLabelFor(kind);
    layer.inPoint = TimeValue::seconds(0.0);
    layer.outPoint = TimeValue::seconds(comp.duration);
    layer.properties = defaultTransform();
    ensureAudioLevel(layer);

    // Topmost first, matching AE and the design's layer ordering.
    comp.layers.insert(comp.layers.begin(), std::move(layer));
    return comp.layers.front();
}

MediaItem& Project::addMedia(std::string path, std::string name, MediaKind kind,
                             double duration, int width, int height, double fps,
                             bool hasAudio) {
    const auto existing = std::find_if(
        media_.begin(), media_.end(),
        [&path](const MediaItem& m) { return m.path == path; });
    if (existing != media_.end()) {
        return *existing;
    }

    MediaItem item;
    item.id = nextId_++;
    item.path = std::move(path);
    item.name = std::move(name);
    item.kind = kind;
    item.duration = duration;
    item.width = width;
    item.height = height;
    item.fps = fps;
    item.hasAudio = hasAudio;
    media_.push_back(std::move(item));
    return media_.back();
}

const MediaItem* Project::findMedia(MediaId id) const noexcept {
    const auto it = std::find_if(media_.begin(), media_.end(),
                                 [id](const MediaItem& m) { return m.id == id; });
    return it == media_.end() ? nullptr : &*it;
}

const MediaItem* Project::findMediaByPath(std::string_view path) const noexcept {
    const auto it = std::find_if(media_.begin(), media_.end(),
                                 [path](const MediaItem& m) { return m.path == path; });
    return it == media_.end() ? nullptr : &*it;
}

std::string Project::pathFor(const Layer& layer) const {
    if (!layer.media.has_value()) {
        return {};
    }
    const MediaItem* item = findMedia(*layer.media);
    return item != nullptr ? item->path : std::string{};
}

void Project::noteUsedId(std::uint64_t id) noexcept {
    if (id >= nextId_) {
        nextId_ = id + 1;
    }
}

Composition* Project::find(CompId comp) noexcept {
    const auto it = std::find_if(comps_.begin(), comps_.end(),
                                 [comp](const Composition& c) { return c.id == comp; });
    return it == comps_.end() ? nullptr : &*it;
}

}  // namespace ruby::core
