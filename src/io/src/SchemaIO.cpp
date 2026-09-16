#include "ruby/io/SchemaIO.h"

#include <nlohmann/json.hpp>

namespace ruby::io {
namespace {

using core::ParamType;
using core::SpatialUnit;
using json = nlohmann::ordered_json;

// Ordered + indented: these files are meant to be diffed.

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

bool unitFrom(const std::string& s, SpatialUnit& out) {
    if (s == "px")                  { out = SpatialUnit::Px; return true; }
    if (s == "percent-of-width")    { out = SpatialUnit::PercentOfWidth; return true; }
    if (s == "percent-of-height")   { out = SpatialUnit::PercentOfHeight; return true; }
    if (s == "percent-of-diagonal") { out = SpatialUnit::PercentOfDiagonal; return true; }
    if (s == "degrees")             { out = SpatialUnit::Degrees; return true; }
    if (s == "percent")             { out = SpatialUnit::Percent; return true; }
    if (s == "normalized")          { out = SpatialUnit::Normalized; return true; }
    if (s == "decibels")            { out = SpatialUnit::Decibels; return true; }
    return false;
}

const char* name(ParamType t) {
    switch (t) {
        case ParamType::Float:  return "float";
        case ParamType::Int:    return "int";
        case ParamType::Bool:   return "bool";
        case ParamType::Color:  return "color";
        case ParamType::Point2: return "point2";
        case ParamType::Enum:   return "enum";
        case ParamType::Text:   return "text";
        case ParamType::Curve:  return "curve";
    }
    return "float";
}

bool typeFrom(const std::string& s, ParamType& out) {
    if (s == "float")  { out = ParamType::Float;  return true; }
    if (s == "int")    { out = ParamType::Int;    return true; }
    if (s == "bool")   { out = ParamType::Bool;   return true; }
    if (s == "color")  { out = ParamType::Color;  return true; }
    if (s == "point2") { out = ParamType::Point2; return true; }
    if (s == "enum")   { out = ParamType::Enum;   return true; }
    if (s == "text")   { out = ParamType::Text;   return true; }
    if (s == "curve")  { out = ParamType::Curve;  return true; }
    return false;
}

json writeRange(const core::ParamRange& r) {
    // Absent limit written as null, not omitted: "unbounded" should be visible in the
    // file, not indistinguishable from a forgotten field.
    json out;
    out["minimum"] = r.minimum.has_value() ? json(*r.minimum) : json(nullptr);
    out["maximum"] = r.maximum.has_value() ? json(*r.maximum) : json(nullptr);
    out["slider"] = json::array({r.slider_min, r.slider_max});
    return out;
}

}  // namespace

std::string schemaToJson(const core::EffectSchema& schema) {
    json out;
    out["id"] = schema.id;
    out["schema"] = schema.schema;
    out["display_name"] = schema.display_name;

    json params = json::array();
    for (const core::ParamSpec& p : schema.params) {
        json j;
        j["key"] = p.key;
        j["label"] = p.label;
        j["order"] = p.order;
        j["type"] = name(p.type);
        j["unit"] = name(p.unit);
        j["default"] = p.default_value;
        if (p.legacy_default.has_value()) {
            j["legacy_default"] = *p.legacy_default;
        }
        j["range"] = writeRange(p.range);
        if (!p.group.empty()) {
            j["group"] = p.group;
        }
        j["introduced_in_schema"] = p.introduced_in_schema;
        params.push_back(std::move(j));
    }
    out["params"] = std::move(params);
    out["retired_keys"] = schema.retired_keys;

    return out.dump(2) + "\n";
}

bool schemaFromJson(const std::string& text, core::EffectSchema& out, std::string* error) {
    const auto fail = [error](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };

    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return fail("not a JSON object");
    }

    core::EffectSchema parsed;
    if (!j.contains("id") || !j.at("id").is_string()) return fail("missing id");
    parsed.id = j.at("id").get<std::string>();
    if (!j.contains("schema") || !j.at("schema").is_number_integer()) {
        return fail("missing schema version");
    }
    parsed.schema = j.at("schema").get<int>();
    parsed.display_name = j.value("display_name", std::string{});

    if (!j.contains("params") || !j.at("params").is_array()) return fail("missing params");
    for (const json& p : j.at("params")) {
        if (!p.is_object()) return fail("a param is not an object");
        core::ParamSpec spec;
        spec.key = p.value("key", std::string{});
        spec.label = p.value("label", std::string{});
        spec.order = p.value("order", 0);
        if (!typeFrom(p.value("type", std::string{"float"}), spec.type)) {
            return fail("unknown param type");
        }
        // Unrecognized unit is refused, not defaulted: silently reading it as `normalized`
        // would reinterpret every stored value of that parameter.
        if (!unitFrom(p.value("unit", std::string{"normalized"}), spec.unit)) {
            return fail("unknown unit");
        }
        spec.default_value = p.value("default", 0.0);
        if (p.contains("legacy_default") && p.at("legacy_default").is_number()) {
            spec.legacy_default = p.at("legacy_default").get<double>();
        }
        if (p.contains("range") && p.at("range").is_object()) {
            const json& r = p.at("range");
            if (r.contains("minimum") && r.at("minimum").is_number()) {
                spec.range.minimum = r.at("minimum").get<double>();
            }
            if (r.contains("maximum") && r.at("maximum").is_number()) {
                spec.range.maximum = r.at("maximum").get<double>();
            }
            if (r.contains("slider") && r.at("slider").is_array() &&
                r.at("slider").size() == 2) {
                spec.range.slider_min = r.at("slider")[0].get<double>();
                spec.range.slider_max = r.at("slider")[1].get<double>();
            }
        }
        spec.group = p.value("group", std::string{});
        spec.introduced_in_schema = p.value("introduced_in_schema", 1);
        parsed.params.push_back(std::move(spec));
    }

    if (j.contains("retired_keys") && j.at("retired_keys").is_array()) {
        for (const json& k : j.at("retired_keys")) {
            if (k.is_string()) parsed.retired_keys.push_back(k.get<std::string>());
        }
    }

    out = std::move(parsed);
    return true;
}

}  // namespace ruby::io
