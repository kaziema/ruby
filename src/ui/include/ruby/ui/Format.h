#pragma once

#include <QString>

#include "ruby/core/Animation.h"
#include "ruby/core/Units.h"

namespace ruby::ui {

// Shared display formatting so the timeline and inspector can't drift apart.

// MM:SS:FF at the composition's frame rate.
[[nodiscard]] QString formatTimecode(double seconds, double fps);

// A property's value at a given time, with its unit's suffix.
[[nodiscard]] QString formatPropertyValue(const core::Property& prop, double seconds,
                                          const core::TimeContext& ctx);

}  // namespace ruby::ui
