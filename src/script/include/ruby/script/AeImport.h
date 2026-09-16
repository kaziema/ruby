#pragma once

#include <string>
#include <vector>

namespace ruby::script {

// Converts AE (JavaScript) expressions to Lua. Scalar one-liners are already valid Lua;
// array literals, var, //, Math., and boolean operators are what breaks.
//
// Not a full JS-to-Lua translator — handles known shapes and refuses to guess at the
// rest; unsure cases come back as a warning with the text left alone.
struct Conversion {
    std::string lua;

    // What was changed, so the result is not a black box.
    std::vector<std::string> notes;

    // What couldn't be handled; non-empty means the output needs human review.
    std::vector<std::string> warnings;

    [[nodiscard]] bool clean() const noexcept { return warnings.empty(); }
};

[[nodiscard]] Conversion convertFromAfterEffects(const std::string& source);

// Whether this looks like AE rather than Lua already, so conversion can be offered
// rather than forced (converting valid Lua would break it).
[[nodiscard]] bool looksLikeAfterEffects(const std::string& source);

}  // namespace ruby::script
