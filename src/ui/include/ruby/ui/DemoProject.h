#pragma once

#include <string>
#include <vector>

#include "ruby/core/Document.h"

namespace ruby::ui::demo {

// TEMPORARY: delete once the app can open a real project file.
// Footage layers use these paths in order, as many as are supplied.
void setMediaPaths(std::vector<std::string> paths);

[[nodiscard]] core::Project sampleProject();

}  // namespace ruby::ui::demo
