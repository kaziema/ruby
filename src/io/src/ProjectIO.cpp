#include "ruby/io/ProjectIO.h"

#include "ruby/core/Transform.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <optional>
#include <sstream>

namespace ruby::io {
namespace {

using json = nlohmann::json;
using namespace ruby::core;

// --- enums as strings ---------------------------------------------------------
// Names, not integers: an integer enum would silently reinterpret older projects if a
// value were ever inserted mid-list.

// Colors are 4 numbers; missing/malformed leaves the value at default rather than
// half-written.
void readRgba(const json& obj, const char* key, Value& into) {
    if (!obj.contains(key) || !obj[key].is_array() || obj[key].size() != 4) {
        return;
    }
    Value parsed;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!obj[key][i].is_number()) {
            return;
        }
        parsed.c[i] = obj[key][i].get<double>();
    }
    parsed.count = 4;
    into = parsed;
}

const char* name(TextAlign a) {
    switch (a) {
        case TextAlign::Left:   return "left";
        case TextAlign::Center: return "center";
        case TextAlign::Right:  return "right";
    }
    return "center";
}
TextAlign textAlign(const std::string& s) {
    if (s == "left")  return TextAlign::Left;
    if (s == "right") return TextAlign::Right;
    return TextAlign::Center;
}

const char* name(LayerKind k) {
    switch (k) {
        case LayerKind::Footage:    return "footage";
        case LayerKind::Precomp:    return "precomp";
        case LayerKind::Text:       return "text";
        case LayerKind::Shape:      return "shape";
        case LayerKind::Solid:      return "solid";
        case LayerKind::Adjustment: return "adjustment";
        case LayerKind::Null:       return "null";
        case LayerKind::Audio:      return "audio";
    }
    return "footage";
}
LayerKind layerKind(const std::string& s) {
    if (s == "precomp")    return LayerKind::Precomp;
    if (s == "text")       return LayerKind::Text;
    if (s == "shape")      return LayerKind::Shape;
    if (s == "solid")      return LayerKind::Solid;
    if (s == "adjustment") return LayerKind::Adjustment;
    if (s == "null")       return LayerKind::Null;
    if (s == "audio")      return LayerKind::Audio;
    return LayerKind::Footage;
}

const char* name(MediaKind k) {
    switch (k) {
        case MediaKind::Video:   return "video";
        case MediaKind::Audio:   return "audio";
        case MediaKind::Image:   return "image";
        case MediaKind::Unknown: return "unknown";
    }
    return "unknown";
}
MediaKind mediaKind(const std::string& s) {
    if (s == "video") return MediaKind::Video;
    if (s == "audio") return MediaKind::Audio;
    if (s == "image") return MediaKind::Image;
    return MediaKind::Unknown;
}

const char* name(BlendMode m) {
    switch (m) {
        case BlendMode::Normal:     return "normal";
        case BlendMode::Add:        return "add";
        case BlendMode::Screen:     return "screen";
        case BlendMode::Multiply:   return "multiply";
        case BlendMode::Overlay:    return "overlay";
        case BlendMode::SoftLight:  return "soft-light";
        case BlendMode::HardLight:  return "hard-light";
        case BlendMode::Difference: return "difference";
        case BlendMode::Lighten:    return "lighten";
        case BlendMode::Darken:     return "darken";
    }
    return "normal";
}
BlendMode blendMode(const std::string& s) {
    if (s == "add")        return BlendMode::Add;
    if (s == "screen")     return BlendMode::Screen;
    if (s == "multiply")   return BlendMode::Multiply;
    if (s == "overlay")    return BlendMode::Overlay;
    if (s == "soft-light") return BlendMode::SoftLight;
    if (s == "hard-light") return BlendMode::HardLight;
    if (s == "difference") return BlendMode::Difference;
    if (s == "lighten")    return BlendMode::Lighten;
    if (s == "darken")     return BlendMode::Darken;
    return BlendMode::Normal;
}

const char* name(LabelColor c) {
    switch (c) {
        case LabelColor::Lavender: return "lavender";
        case LabelColor::Aqua:     return "aqua";
        case LabelColor::Gray:     return "gray";
        case LabelColor::Green:    return "green";
    }
    return "gray";
}
LabelColor labelColor(const std::string& s) {
    if (s == "lavender") return LabelColor::Lavender;
    if (s == "aqua")     return LabelColor::Aqua;
    if (s == "green")    return LabelColor::Green;
    return LabelColor::Gray;
}

const char* name(SpatialUnit u) {
    switch (u) {
        case SpatialUnit::Px:                return "px";
        case SpatialUnit::PercentOfWidth:    return "percent-of-width";
        case SpatialUnit::PercentOfHeight:   return "percent-of-height";
        case SpatialUnit::PercentOfDiagonal: return "percent-of-diagonal";
        case SpatialUnit::Degrees:           return "degrees";
        case SpatialUnit::Percent:           return "percent";
        case SpatialUnit::Normalized:        return "normalized";
        case SpatialUnit::Decibels:          return "decibels";
    }
    return "normalized";
}
SpatialUnit spatialUnit(const std::string& s) {
    if (s == "px")                  return SpatialUnit::Px;
    if (s == "percent-of-width")    return SpatialUnit::PercentOfWidth;
    if (s == "percent-of-height")   return SpatialUnit::PercentOfHeight;
    if (s == "percent-of-diagonal") return SpatialUnit::PercentOfDiagonal;
    if (s == "degrees")             return SpatialUnit::Degrees;
    if (s == "percent")             return SpatialUnit::Percent;
    if (s == "decibels")            return SpatialUnit::Decibels;
    return SpatialUnit::Normalized;
}

const char* name(MaskShape s) {
    switch (s) {
        case MaskShape::Rectangle: return "rectangle";
        case MaskShape::Ellipse:   return "ellipse";
    }
    return "rectangle";
}
// Unknown means a newer build wrote it (a Bezier mask, say). Refused, never guessed:
// reading it as a rectangle would silently change what the effect covers.
std::optional<MaskShape> maskShape(const std::string& s) {
    if (s == "rectangle") return MaskShape::Rectangle;
    if (s == "ellipse")   return MaskShape::Ellipse;
    return std::nullopt;
}

const char* name(MaskMode m) {
    switch (m) {
        case MaskMode::Add:       return "add";
        case MaskMode::Subtract:  return "subtract";
        case MaskMode::Intersect: return "intersect";
        case MaskMode::None:      return "none";
    }
    return "none";
}
MaskMode maskMode(const std::string& s) {
    if (s == "add")       return MaskMode::Add;
    if (s == "subtract")  return MaskMode::Subtract;
    if (s == "intersect") return MaskMode::Intersect;
    return MaskMode::None;  // unknown mode renders as off, which is visible, not wrong
}

const char* name(TimeMode m) {
    switch (m) {
        case TimeMode::Beats:   return "beats";
        case TimeMode::Seconds: return "seconds";
        case TimeMode::Frames:  return "frames";
    }
    return "seconds";
}
TimeMode timeMode(const std::string& s) {
    if (s == "beats")  return TimeMode::Beats;
    if (s == "frames") return TimeMode::Frames;
    return TimeMode::Seconds;
}

const char* name(Interpolation i) {
    switch (i) {
        case Interpolation::Linear: return "linear";
        case Interpolation::Bezier: return "bezier";
        case Interpolation::Hold:   return "hold";
    }
    return "bezier";
}
Interpolation interpolation(const std::string& s) {
    if (s == "linear") return Interpolation::Linear;
    if (s == "hold")   return Interpolation::Hold;
    return Interpolation::Bezier;
}

const char* name(MarkerLane l) {
    switch (l) {
        case MarkerLane::Beat:     return "beat";
        case MarkerLane::Downbeat: return "downbeat";
        case MarkerLane::Vocal:    return "vocal";
        case MarkerLane::User:     return "user";
    }
    return "beat";
}
MarkerLane markerLane(const std::string& s) {
    if (s == "downbeat") return MarkerLane::Downbeat;
    if (s == "vocal")    return MarkerLane::Vocal;
    if (s == "user")     return MarkerLane::User;
    return MarkerLane::Beat;
}

// --- write ---------------------------------------------------------------------

json write(const TimeValue& t) {
    return json{{"mode", name(t.mode)}, {"value", t.value}};
}

json write(const Value& v) {
    json out = json::array();
    for (int i = 0; i < v.count; ++i) {
        out.push_back(v.c[static_cast<std::size_t>(i)]);
    }
    return out;
}

json write(const Keyframe& k) {
    json out{{"time", write(k.time)},
             {"value", write(k.value)},
             {"interp", name(k.interp)}};
    // Only write easing that does something; zeroes are noise.
    if (k.easeIn != 0.0)     out["easeIn"] = k.easeIn;
    if (k.easeOut != 0.0)    out["easeOut"] = k.easeOut;
    if (k.overshoot != 0.0)  out["overshoot"] = k.overshoot;
    return out;
}

json write(const Property& p) {
    json out{{"key", p.key}, {"label", p.label}, {"unit", name(p.unit)},
             {"value", write(p.staticValue)}};
    if (!p.group.empty() && p.group != "Transform") out["group"] = p.group;
    if (p.expression.has_value())                   out["expression"] = *p.expression;
    if (p.expanded)                                 out["expanded"] = true;
    if (!p.keys.empty()) {
        json keys = json::array();
        for (const Keyframe& k : p.keys) keys.push_back(write(k));
        out["keys"] = std::move(keys);
    }
    return out;
}

json write(const Mask& m) {
    json params = json::array();
    for (const Property& p : m.params) params.push_back(write(p));
    json out{{"name", m.name}, {"shape", name(m.shape)}, {"mode", name(m.mode)},
             {"params", std::move(params)}};
    if (m.inverted) out["inverted"] = true;
    return out;
}

json write(const EffectInstance& e) {
    json params = json::array();
    for (const Property& p : e.params) params.push_back(write(p));
    // `schema` lets an older file migrate forward; never drop it.
    json out{{"effect", e.effectId}, {"schema", e.schema},
             {"name", e.displayName}, {"enabled", e.enabled},
             {"expanded", e.expanded}, {"params", std::move(params)}};
    if (!e.masks.empty()) {
        json masks = json::array();
        for (const Mask& m : e.masks) masks.push_back(write(m));
        out["masks"] = std::move(masks);
    }
    return out;
}

json write(const Layer& l) {
    json out{{"id", l.id}, {"name", l.name}, {"kind", name(l.kind)},
             {"label", name(l.label)}, {"in", write(l.inPoint)},
             {"out", write(l.outPoint)}, {"blend", name(l.blend)},
             {"enabled", l.enabled}, {"audioEnabled", l.audioEnabled},
             {"solo", l.solo}, {"locked", l.locked}, {"expanded", l.expanded},
             {"transformExpanded", l.transformExpanded}};


    // Text-only fields.
    if (l.kind == LayerKind::Text) {
        out["text"] = l.text;
        out["fontFamily"] = l.fontFamily;
        out["fontSize"] = l.fontSize;
        out["tracking"] = l.tracking;
        out["lineHeight"] = l.lineHeight;
        out["textAlign"] = name(l.textAlign);
        out["textColor"] = {l.textColor.c[0], l.textColor.c[1], l.textColor.c[2],
                            l.textColor.c[3]};
        out["strokeColor"] = {l.strokeColor.c[0], l.strokeColor.c[1], l.strokeColor.c[2],
                              l.strokeColor.c[3]};
        out["strokeWidth"] = l.strokeWidth;
    }

    // Solid-only fields.
    if (l.kind == LayerKind::Solid) {
        out["solidColor"] = {l.solidColor.c[0], l.solidColor.c[1], l.solidColor.c[2],
                             l.solidColor.c[3]};
        out["solidWidth"] = l.solidWidth;
        out["solidHeight"] = l.solidHeight;
    }

    if (l.parent.has_value()) out["parent"] = *l.parent;
    if (l.source.has_value()) out["source"] = *l.source;
    if (l.media.has_value())  out["media"] = *l.media;

    json props = json::array();
    for (const Property& p : l.properties) props.push_back(write(p));
    out["properties"] = std::move(props);

    if (!l.effects.empty()) {
        json fx = json::array();
        for (const EffectInstance& e : l.effects) fx.push_back(write(e));
        out["effects"] = std::move(fx);
    }
    // Waveform is derived from media and cheaply recomputed; not written.
    return out;
}

json write(const RhythmMap& r) {
    json markers = json::array();
    for (const Marker& m : r.markers()) {
        markers.push_back(json{{"t", m.seconds}, {"lane", name(m.lane)},
                               {"strength", m.strength}, {"index", m.index}});
    }
    return json{{"bpm", r.bpm()}, {"markers", std::move(markers)}};
}

json write(const Composition& c) {
    json layers = json::array();
    for (const Layer& l : c.layers) layers.push_back(write(l));
    json out{{"id", c.id}, {"name", c.name}, {"width", c.width},
             {"height", c.height}, {"fps", c.fps}, {"duration", c.duration},
             {"layers", std::move(layers)}, {"rhythm", write(c.rhythm)}};
    // Only when set; a pair of zeroes would read as a zero-length work area, not absence.
    if (c.hasWorkArea()) {
        out["workIn"] = write(c.workIn);
        out["workOut"] = write(c.workOut);
    }
    return out;
}

json write(const MediaItem& m) {
    return json{{"id", m.id}, {"path", m.path}, {"name", m.name},
                {"kind", name(m.kind)}, {"duration", m.duration},
                {"width", m.width}, {"height", m.height}, {"fps", m.fps},
                {"hasAudio", m.hasAudio}};
}

// --- read ----------------------------------------------------------------------
// Every read is total: missing/wrong-typed fields fall back rather than throw, so one
// odd field doesn't refuse the whole project.

template <typename T>
T get(const json& j, const char* key, T fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    try {
        return j.at(key).get<T>();
    } catch (...) {
        return fallback;
    }
}

std::string str(const json& j, const char* key, const std::string& fallback = {}) {
    return get<std::string>(j, key, fallback);
}

TimeValue readTime(const json& j) {
    if (!j.is_object()) return TimeValue::seconds(0.0);
    return TimeValue{timeMode(str(j, "mode", "seconds")), get<double>(j, "value", 0.0)};
}

Value readValue(const json& j) {
    Value v;
    if (!j.is_array()) return v;
    v.count = 0;
    for (const auto& item : j) {
        if (v.count >= 4) break;
        v.c[static_cast<std::size_t>(v.count)] = item.is_number() ? item.get<double>() : 0.0;
        ++v.count;
    }
    if (v.count == 0) v.count = 1;
    return v;
}

Keyframe readKeyframe(const json& j) {
    Keyframe k;
    k.time = readTime(j.is_object() && j.contains("time") ? j.at("time") : json{});
    k.value = readValue(j.is_object() && j.contains("value") ? j.at("value") : json{});
    k.interp = interpolation(str(j, "interp", "bezier"));
    k.easeIn = get<double>(j, "easeIn", 0.0);
    k.easeOut = get<double>(j, "easeOut", 0.0);
    k.overshoot = get<double>(j, "overshoot", 0.0);
    return k;
}

Property readProperty(const json& j) {
    Property p;
    p.key = str(j, "key");
    p.label = str(j, "label", p.key);
    p.group = str(j, "group", "Transform");
    p.unit = spatialUnit(str(j, "unit", "normalized"));
    p.staticValue = readValue(j.is_object() && j.contains("value") ? j.at("value") : json{});
    p.expanded = get<bool>(j, "expanded", false);
    if (j.is_object() && j.contains("expression") && j.at("expression").is_string()) {
        p.expression = j.at("expression").get<std::string>();
    }
    if (j.is_object() && j.contains("keys") && j.at("keys").is_array()) {
        for (const auto& k : j.at("keys")) p.keys.push_back(readKeyframe(k));
    }
    return p;
}

}  // namespace

// --- public --------------------------------------------------------------------

std::string toJson(const Project& project) {
    json media = json::array();
    for (const MediaItem& m : project.media()) media.push_back(write(m));

    json comps = json::array();
    for (const Composition& c : project.compositions()) comps.push_back(write(c));

    const json doc{{"format", "ruby-project"},
                   {"schema", kProjectSchema},
                   {"media", std::move(media)},
                   {"compositions", std::move(comps)}};
    return doc.dump(2) + "\n";
}

LoadReport fromJson(Project& project, const std::string& text) {
    LoadReport report;

    json doc;
    try {
        doc = json::parse(text);
    } catch (const std::exception& e) {
        report.error = std::string("not valid JSON: ") + e.what();
        return report;
    }
    if (!doc.is_object() || str(doc, "format") != "ruby-project") {
        report.error = "not a Ruby project file";
        return report;
    }

    const int schema = get<int>(doc, "schema", 0);
    if (schema > kProjectSchema) {
        // Refuse rather than open: an older build would drop what it doesn't understand,
        // and a subsequent save would destroy it.
        report.error = "saved by a newer version of Ruby (file schema " +
                       std::to_string(schema) + ", this build reads " +
                       std::to_string(kProjectSchema) + ")";
        return report;
    }

    // Build into a fresh project so a failure halfway doesn't leave a half-loaded document.
    Project loaded;

    if (doc.contains("media") && doc.at("media").is_array()) {
        for (const auto& m : doc.at("media")) {
            MediaItem& item = loaded.addMedia(
                str(m, "path"), str(m, "name"), mediaKind(str(m, "kind", "unknown")),
                get<double>(m, "duration", 0.0), get<int>(m, "width", 0),
                get<int>(m, "height", 0), get<double>(m, "fps", 0.0),
                get<bool>(m, "hasAudio", false));
            // Preserve ids so layer references survive the round trip.
            item.id = get<MediaId>(m, "id", item.id);
            loaded.noteUsedId(item.id);
        }
    }

    if (doc.contains("compositions") && doc.at("compositions").is_array()) {
        for (const auto& c : doc.at("compositions")) {
            Composition& comp = loaded.addComposition(
                str(c, "name", "Comp"), get<int>(c, "width", 1080),
                get<int>(c, "height", 1920), get<double>(c, "fps", 30.0),
                get<double>(c, "duration", 12.0));
            comp.id = get<CompId>(c, "id", comp.id);
            loaded.noteUsedId(comp.id);

            if (c.contains("workIn") && c.contains("workOut")) {
                comp.workIn = readTime(c.at("workIn"));
                comp.workOut = readTime(c.at("workOut"));
            }

            if (c.contains("rhythm") && c.at("rhythm").is_object()) {
                const json& r = c.at("rhythm");
                comp.rhythm.setTempo(get<double>(r, "bpm", 0.0));
                if (r.contains("markers") && r.at("markers").is_array()) {
                    std::vector<Marker> byLane[4];
                    for (const auto& m : r.at("markers")) {
                        Marker marker;
                        marker.seconds = get<double>(m, "t", 0.0);
                        marker.lane = markerLane(str(m, "lane", "beat"));
                        marker.strength = get<float>(m, "strength", 1.0f);
                        marker.index = get<int>(m, "index", 0);
                        byLane[static_cast<int>(marker.lane)].push_back(marker);
                    }
                    for (int i = 0; i < 4; ++i) {
                        if (!byLane[i].empty()) {
                            comp.rhythm.setLane(static_cast<MarkerLane>(i), byLane[i]);
                        }
                    }
                }
            }

            if (!c.contains("layers") || !c.at("layers").is_array()) {
                continue;
            }
            for (const auto& l : c.at("layers")) {
                Layer layer;
                layer.id = get<LayerId>(l, "id", 0);
                layer.name = str(l, "name", "Layer");
                layer.kind = layerKind(str(l, "kind", "footage"));
                layer.label = labelColor(str(l, "label", "gray"));
                layer.inPoint = readTime(l.contains("in") ? l.at("in") : json{});
                layer.outPoint = readTime(l.contains("out") ? l.at("out") : json{});
                layer.blend = blendMode(str(l, "blend", "normal"));
                layer.enabled = get<bool>(l, "enabled", true);
                // Defaults on so pre-audio-switch projects open with sound audible.
                layer.audioEnabled = get<bool>(l, "audioEnabled", true);

                readRgba(l, "solidColor", layer.solidColor);
                layer.text = get<std::string>(l, "text", std::string());
                layer.fontFamily = get<std::string>(l, "fontFamily", "Helvetica");
                layer.fontSize = get<double>(l, "fontSize", 72.0);
                layer.tracking = get<double>(l, "tracking", 0.0);
                layer.lineHeight = get<double>(l, "lineHeight", 1.2);
                layer.textAlign = textAlign(get<std::string>(l, "textAlign", "center"));
                layer.strokeWidth = get<double>(l, "strokeWidth", 0.0);
                readRgba(l, "textColor", layer.textColor);
                readRgba(l, "strokeColor", layer.strokeColor);

                layer.solidWidth = get<int>(l, "solidWidth", 0);
                layer.solidHeight = get<int>(l, "solidHeight", 0);
                layer.solo = get<bool>(l, "solo", false);
                layer.locked = get<bool>(l, "locked", false);
                layer.expanded = get<bool>(l, "expanded", false);
                layer.transformExpanded = get<bool>(l, "transformExpanded", true);
                if (l.contains("parent") && l.at("parent").is_number()) {
                    layer.parent = l.at("parent").get<LayerId>();
                }
                if (l.contains("source") && l.at("source").is_number()) {
                    layer.source = l.at("source").get<CompId>();
                }
                if (l.contains("media") && l.at("media").is_number()) {
                    const auto id = l.at("media").get<MediaId>();
                    if (loaded.findMedia(id) != nullptr) {
                        layer.media = id;
                    } else {
                        // Dangling reference: keep the layer, drop the link rather than
                        // losing the layer.
                        report.notes.push_back("layer \"" + layer.name +
                                               "\" referenced media that is not in the "
                                               "project; it was unlinked");
                    }
                }

                if (l.contains("properties") && l.at("properties").is_array()) {
                    for (const auto& p : l.at("properties")) {
                        layer.properties.push_back(readProperty(p));
                    }
                }
                if (l.contains("effects") && l.at("effects").is_array()) {
                    for (const auto& e : l.at("effects")) {
                        EffectInstance fx;
                        fx.effectId = str(e, "effect");
                        fx.schema = get<int>(e, "schema", 1);
                        fx.displayName = str(e, "name", fx.effectId);
                        fx.enabled = get<bool>(e, "enabled", true);
                        fx.expanded = get<bool>(e, "expanded", true);
                        if (e.contains("params") && e.at("params").is_array()) {
                            for (const auto& p : e.at("params")) {
                                fx.params.push_back(readProperty(p));
                            }
                        }
                        if (e.contains("masks") && e.at("masks").is_array()) {
                            for (const auto& mj : e.at("masks")) {
                                const std::string shapeName = str(mj, "shape", "rectangle");
                                const std::optional<MaskShape> shape = maskShape(shapeName);
                                if (!shape.has_value()) {
                                    report.notes.push_back(
                                        "layer \"" + layer.name + "\": skipped a \"" +
                                        shapeName + "\" mask this version can't draw");
                                    continue;
                                }
                                // Built from the canonical definition, then the saved
                                // values laid on top: ranges stay schema-owned.
                                Mask m = makeMask(*shape, str(mj, "name", "Mask"));
                                m.mode = maskMode(str(mj, "mode", "add"));
                                m.inverted = get<bool>(mj, "inverted", false);
                                if (mj.contains("params") && mj.at("params").is_array()) {
                                    for (const auto& pj : mj.at("params")) {
                                        const Property stored = readProperty(pj);
                                        if (Property* p = m.find(stored.key)) {
                                            p->staticValue = stored.staticValue;
                                            p->keys = stored.keys;
                                            p->expression = stored.expression;
                                            p->expanded = stored.expanded;
                                        }
                                    }
                                }
                                fx.masks.push_back(std::move(m));
                            }
                        }
                        layer.effects.push_back(std::move(fx));
                    }
                }
                comp.layers.push_back(std::move(layer));
                loaded.noteUsedId(comp.layers.back().id);
            }
        }
    }

    // Break any parent loop and report it. The render path survives a cycle but the
    // document would stay broken, so fix it here where the whole graph is visible.
    for (Composition& comp : loaded.compositions()) {
        for (Layer& layer : comp.layers) {
            if (!layer.parent.has_value() || !hasParentCycle(comp, layer.id)) {
                continue;
            }
            report.notes.push_back("layer \"" + layer.name +
                                   "\" was part of a parent loop; its parent was cleared");
            layer.parent.reset();
        }
    }

    project = std::move(loaded);
    report.ok = true;
    return report;
}

bool save(const Project& project, const std::string& path, std::string* error) {
    // Write to a temp file then rename into place, so a crash mid-write can't truncate
    // the existing file.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error != nullptr) *error = "could not open " + temp + " for writing";
            return false;
        }
        out << toJson(project);
        if (!out) {
            if (error != nullptr) *error = "failed while writing " + temp;
            return false;
        }
    }
    std::remove(path.c_str());
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        if (error != nullptr) *error = "could not move the new file into place";
        return false;
    }
    return true;
}

LoadReport load(Project& project, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LoadReport report;
        report.error = "could not open " + path;
        return report;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return fromJson(project, text.str());
}

}  // namespace ruby::io
