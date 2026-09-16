// schemas/ is the record of what has shipped. These tests make it load-bearing: an
// effect changed in code without updating its file fails the build.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "ruby/core/Identity.h"
#include "ruby/engine/EffectRegistry.h"
#include "ruby/io/SchemaIO.h"

using namespace ruby;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

fs::path schemaDir() {
    // Passed in by CTest so the test does not care where it is run from.
    const char* root = std::getenv("RUBY_SCHEMA_DIR");
    return root != nullptr ? fs::path(root) : fs::path("schemas");
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Every registered effect has a file and they agree byte for byte (not field by field,
// so the diff shows any change, even formatting).
void every_effect_matches_its_recorded_schema() {
    const fs::path dir = schemaDir() / "effects";
    check(fs::exists(dir), "schemas/effects exists");
    if (!fs::exists(dir)) {
        return;
    }

    std::set<std::string> onDisk;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".json") {
            onDisk.insert(entry.path().stem().string());
        }
    }

    std::set<std::string> registered;
    for (const auto& def : engine::EffectRegistry::instance().all()) {
        registered.insert(def.schema.id);

        const fs::path file = dir / (def.schema.id + ".json");
        if (!fs::exists(file)) {
            check(false, "no recorded schema for " + def.schema.id +
                             " (run gen_schemas and review the diff)");
            continue;
        }
        const std::string recorded = readFile(file);
        const std::string current = io::schemaToJson(def.schema);
        check(recorded == current,
              def.schema.id +
                  " has changed since it was recorded; if a parameter changed, bump the "
                  "schema version and write a migration, then regenerate the file");
    }

    for (const std::string& id : onDisk) {
        check(registered.count(id) == 1,
              "schemas/effects/" + id + ".json has no registered effect; an effect id is "
              "immortal, so a file should outlive the code, not the other way round");
    }
}

// The written form must round-trip, or a loaded project can't be compared against it.
void a_recorded_schema_parses_back_to_itself() {
    for (const auto& def : engine::EffectRegistry::instance().all()) {
        const std::string text = io::schemaToJson(def.schema);
        core::EffectSchema back;
        std::string error;
        if (!io::schemaFromJson(text, back, &error)) {
            check(false, def.schema.id + " failed to parse back: " + error);
            continue;
        }
        check(io::schemaToJson(back) == text, def.schema.id + " round trips unchanged");
        check(validate(back).empty(), def.schema.id + " is still valid after a round trip");
    }
}

// An unknown unit must be refused, not defaulted (would silently reinterpret stored values).
void an_unknown_unit_is_refused() {
    const std::string text = R"({
      "id": "core.test.thing", "schema": 1, "display_name": "Thing",
      "params": [{"key":"a","label":"A","order":0,"type":"float",
                  "unit":"furlongs","default":0.0,"introduced_in_schema":1}],
      "retired_keys": []
    })";
    core::EffectSchema out;
    std::string error;
    check(!io::schemaFromJson(text, out, &error), "a schema with an unknown unit is refused");
    check(error.find("unit") != std::string::npos, "and says which part it refused");
}

void a_malformed_schema_does_not_half_load() {
    core::EffectSchema out;
    out.id = "untouched";
    check(!io::schemaFromJson("{ not json", out), "garbage is refused");
    check(!io::schemaFromJson(R"({"id":"a.b"})", out), "a schema with no version is refused");
    check(out.id == "untouched", "and the target is left alone on failure");
}

// Nothing loads presets yet; check the JSON Schema docs are well formed and the worked
// example agrees on required keys, enums, and identity.
void the_preset_schemas_are_well_formed() {
    for (const char* name : {"manifest.schema.json", "content.schema.json"}) {
        const fs::path p = schemaDir() / "preset" / name;
        check(fs::exists(p), std::string("schemas/preset/") + name + " exists");
        if (!fs::exists(p)) continue;
        const json j = json::parse(readFile(p), nullptr, false);
        check(!j.is_discarded(), std::string(name) + " is valid JSON");
        if (j.is_discarded()) continue;
        check(j.contains("$schema"), std::string(name) + " declares its dialect");
        check(j.value("type", std::string{}) == "object",
              std::string(name) + " describes an object");
        check(j.contains("required") && j.at("required").is_array(),
              std::string(name) + " says what is required");
    }
}

void the_example_preset_agrees_with_the_manifest_schema() {
    const json schema =
        json::parse(readFile(schemaDir() / "preset" / "manifest.schema.json"), nullptr, false);
    const json example =
        json::parse(readFile(schemaDir() / "preset" / "example" / "manifest.json"), nullptr,
                    false);
    check(!schema.is_discarded() && !example.is_discarded(), "both parse");
    if (schema.is_discarded() || example.is_discarded()) return;

    for (const json& key : schema.at("required")) {
        check(example.contains(key.get<std::string>()),
              "the example manifest has required key \"" + key.get<std::string>() + "\"");
    }

    // additionalProperties is false, so an undescribed key in the example is a real error.
    const json& props = schema.at("properties");
    for (const auto& [key, value] : example.items()) {
        (void)value;
        check(props.contains(key),
              "the example manifest's \"" + key + "\" is described by the schema");
    }

    check(example.value("format", std::string{}) == "ruby.preset",
          "the example declares the format, so a file is identifiable without its extension");
    const std::string kind = example.value("kind", std::string{});
    check(kind == "set" || kind == "pack", "kind is one of the two the loader knows");
}

void the_example_content_agrees_with_its_schema() {
    const json schema =
        json::parse(readFile(schemaDir() / "preset" / "content.schema.json"), nullptr, false);
    const json example =
        json::parse(readFile(schemaDir() / "preset" / "example" / "content.json"), nullptr,
                    false);
    check(!schema.is_discarded() && !example.is_discarded(), "both parse");
    if (schema.is_discarded() || example.is_discarded()) return;

    for (const json& key : schema.at("required")) {
        check(example.contains(key.get<std::string>()),
              "the example content has required key \"" + key.get<std::string>() + "\"");
    }
    check(example.at("layers").is_array() && !example.at("layers").empty(),
          "a fragment is at least one layer");

    // The example must reference a real effect, or it teaches a preset that can't load.
    for (const json& layer : example.at("layers")) {
        for (const json& fx : layer.value("effects", json::array())) {
            const std::string id = fx.value("effect", std::string{});
            check(engine::EffectRegistry::instance().find(id) != nullptr,
                  "the example uses a real effect: " + id);
        }
    }

    // And the times are authored in beats, which is the whole point of D3 and the reason
    // a preset survives being dropped on a song at a different tempo.
    check(example.at("layers")[0].at("in").at("mode") == "beats",
          "the example is authored in beats, not frames");
}

}  // namespace

int main() {
    every_effect_matches_its_recorded_schema();
    a_recorded_schema_parses_back_to_itself();
    an_unknown_unit_is_refused();
    a_malformed_schema_does_not_half_load();
    the_preset_schemas_are_well_formed();
    the_example_preset_agrees_with_the_manifest_schema();
    the_example_content_agrees_with_its_schema();

    if (failures == 0) {
        std::puts("schemas: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
