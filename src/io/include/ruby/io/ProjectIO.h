#pragma once

#include <string>
#include <vector>

#include "ruby/core/Document.h"

namespace ruby::io {

// Reading and writing `.rbypr` project files. Plain JSON, not a binary/zip container:
// a project carries no assets, and JSON stays greppable, diffable, and recoverable.

// What went wrong, or what was quietly fixed up.
struct LoadReport {
    bool ok = false;
    std::string error;             // set when the file could not be read at all
    std::vector<std::string> notes;  // recoverable problems, in the order they happened

    [[nodiscard]] bool clean() const noexcept { return ok && notes.empty(); }
};

// Bumped whenever the on-disk shape changes; the loader uses it to pick migrations.
inline constexpr int kProjectSchema = 1;

[[nodiscard]] bool save(const core::Project& project, const std::string& path,
                        std::string* error = nullptr);

// On failure, `project` is left untouched.
[[nodiscard]] LoadReport load(core::Project& project, const std::string& path);

// Exposed for tests and for string round-tripping.
[[nodiscard]] std::string toJson(const core::Project& project);
[[nodiscard]] LoadReport fromJson(core::Project& project, const std::string& text);

}  // namespace ruby::io
