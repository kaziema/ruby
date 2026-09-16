// Every built-in effect runs through validate() — breaking identity rules would silently
// break every preset authored against it, so it fails the build instead.

#include <cstdio>
#include <cstdlib>

#include "ruby/core/Identity.h"
#include "ruby/engine/EffectRegistry.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main() {
    const engine::EffectRegistry& registry = engine::EffectRegistry::instance();

    check(!registry.all().empty(), "the registry has effects in it");

    for (const engine::EffectDef& def : registry.all()) {
        const auto problems = core::validate(def.schema);
        for (const core::SchemaProblem& problem : problems) {
            std::fprintf(stderr, "FAIL: %s: %s\n", problem.where.c_str(),
                         problem.message.c_str());
            ++failures;
        }

        check(!def.shader.empty(), "the effect has a shader");
        check(def.schema.params.size() <=
                  static_cast<std::size_t>(engine::EffectDef::kMaxParams),
              "the effect fits in the uniform block");

        // Params are packed positionally; a gap/duplicate order misplaces a shader slot.
        for (std::size_t i = 0; i < def.schema.params.size(); ++i) {
            check(def.schema.params[i].order == static_cast<int>(i),
                  "parameter order matches its packing slot");
        }
    }

    const engine::EffectDef* grade = registry.find("core.color.grade");
    check(grade != nullptr, "the grade effect is findable by its id");
    check(registry.find("core.does.not.exist") == nullptr,
          "an unknown id returns nothing rather than a default");

    core::EffectInstance instance = registry.instantiate("core.color.grade");
    check(instance.effectId == "core.color.grade", "the instance carries the effect id");
    check(instance.schema >= 1, "the instance records the schema version it was built at");
    check(instance.params.size() == 3, "the instance has every parameter");
    check(instance.find("exposure") != nullptr, "parameters are findable by key");

    // Defaults must be identity, or dropping an effect changes the picture untouched.
    const core::Property* exposure = instance.find("exposure");
    const core::Property* contrast = instance.find("contrast");
    const core::Property* saturation = instance.find("saturation");
    check(exposure != nullptr && exposure->staticValue.x() == 0.0,
          "exposure defaults to no change");
    check(contrast != nullptr && contrast->staticValue.x() == 100.0,
          "contrast defaults to no change");
    check(saturation != nullptr && saturation->staticValue.x() == 100.0,
          "saturation defaults to no change");

    check(registry.instantiate("core.nope").params.empty(),
          "instantiating an unknown effect yields nothing rather than a broken instance");

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("effects: all checks passed");
    return EXIT_SUCCESS;
}
