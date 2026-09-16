#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ruby/core/Expressions.h"
#include "ruby/script/Sandbox.h"

namespace ruby::script {

// Plugs the sandbox into core's expression seam. One per thread, installed explicitly
// (never auto-started) since some callers — a migration tool, a test — should have none.
class LuaHost : public core::ExpressionHost {
public:
    ~LuaHost() override;

    [[nodiscard]] static std::unique_ptr<LuaHost> create();

    [[nodiscard]] bool evaluate(const core::Property& prop, double seconds,
                                const core::TimeContext& ctx, const core::Value& fallback,
                                std::uint64_t seed, core::Value& out) override;

    // Failed expressions, most recent first, capped — shown next to the property since
    // a silent fallback to the keyframed value would otherwise be invisible.
    struct Failure {
        std::string source;
        std::string error;
    };
    [[nodiscard]] const std::vector<Failure>& failures() const noexcept;
    void clearFailures();

    [[nodiscard]] Sandbox& sandbox() noexcept { return *box_; }

private:
    LuaHost();

    std::unique_ptr<Sandbox> box_;
    std::vector<Failure> failures_;
};

// Installs `host` for the calling thread, removes it on destruction — a bare setter
// risks leaving core with a dangling pointer if clearing is forgotten.
class ScopedHost {
public:
    explicit ScopedHost(core::ExpressionHost* host);
    ~ScopedHost();

    ScopedHost(const ScopedHost&) = delete;
    ScopedHost& operator=(const ScopedHost&) = delete;

private:
    core::ExpressionHost* previous_ = nullptr;
};

}  // namespace ruby::script
