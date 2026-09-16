#pragma once

#include <string>
#include <vector>

#include "ruby/core/Identity.h"

namespace ruby::io {

// Effect schemas as written to disk (schemas/effects/*.json): the record of what has
// shipped, checked against the registry's idea of what ships today so version bumps and
// migrations aren't forgotten. Not generated at build time — needs to show up in diffs.

[[nodiscard]] std::string schemaToJson(const core::EffectSchema& schema);

// Parses one back. Returns false and leaves `out` untouched on malformed input; `error`
// gets a reason when supplied.
[[nodiscard]] bool schemaFromJson(const std::string& text, core::EffectSchema& out,
                                  std::string* error = nullptr);

}  // namespace ruby::io
