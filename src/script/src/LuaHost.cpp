#include "ruby/script/LuaHost.h"

#include <algorithm>

namespace ruby::script {
namespace {

// Enough to surface what's broken without the list growing faster than anyone reads it.
constexpr std::size_t kMaxFailures = 32;

}  // namespace

LuaHost::LuaHost() = default;
LuaHost::~LuaHost() = default;

std::unique_ptr<LuaHost> LuaHost::create() {
    std::unique_ptr<LuaHost> host(new LuaHost());
    host->box_ = Sandbox::create();
    if (host->box_ == nullptr) {
        return nullptr;
    }
    return host;
}

bool LuaHost::evaluate(const core::Property& prop, double seconds,
                       const core::TimeContext& ctx, const core::Value& fallback,
                       std::uint64_t seed, core::Value& out) {
    const std::string& source = *prop.expression;

    box_->setInputs(seconds, fallback, seed);
    box_->setProperty(&prop, &ctx);
    const Sandbox::Outcome outcome = box_->evaluate(source);

    // Cleared after each run; holding it between calls risks reading a deleted layer.
    box_->setProperty(nullptr, nullptr);
    if (!outcome.ok) {
        // Recorded once per distinct expression, not once per frame.
        const bool known = std::any_of(
            failures_.begin(), failures_.end(),
            [&source](const Failure& f) { return f.source == source; });
        if (!known && failures_.size() < kMaxFailures) {
            failures_.push_back({source, outcome.error});
        }
        return false;
    }
    out = outcome.value;
    return true;
}

const std::vector<LuaHost::Failure>& LuaHost::failures() const noexcept {
    return failures_;
}

void LuaHost::clearFailures() { failures_.clear(); }

ScopedHost::ScopedHost(core::ExpressionHost* host)
    : previous_(core::expressionHost()) {
    core::setExpressionHost(host);
}

ScopedHost::~ScopedHost() { core::setExpressionHost(previous_); }

}  // namespace ruby::script
