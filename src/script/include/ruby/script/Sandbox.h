#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "ruby/core/Document.h"

namespace ruby::script {

// Sandboxed Lua interpreter that expressions run inside — an allowlist of what's let in,
// not Lua with things removed, since untrusted expressions arrive from preset packs.
//
// Guarantees: no reach outside the process (no fs/network/subprocess/runtime code
// loading); no infinite loops (instruction budget); no non-determinism (no clock, no
// unseeded random — renders must reproduce exactly).
//
// Not thread safe by design: one Sandbox per thread, no internal lock (would sit in the
// hot path of every animated property).
class Sandbox {
public:
    ~Sandbox();

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    // Null only if Lua fails to start (in practice, out of memory).
    [[nodiscard]] static std::unique_ptr<Sandbox> create();

    struct Outcome {
        bool ok = false;
        core::Value value;
        std::string error;  // set when ok is false, phrased for a user to read

        // True when stopped for exceeding the instruction budget, not for failing —
        // distinct from a syntax error, which is the author's mistake.
        bool exhausted = false;
    };

    // Evaluates `source`; a number becomes a scalar, a table of 2-4 numbers a vector.
    [[nodiscard]] Outcome evaluate(const std::string& source);

    // Time, the property's own value, and a seed stable per layer/property, constant
    // across frames — the seed is what makes wiggle deterministic per layer.
    void setInputs(double time, const core::Value& value, std::uint64_t seed);

    // The property being evaluated, for expression functions (loopOut etc.) that read
    // its keyframes. Borrowed for the next evaluate() call only.
    void setProperty(const core::Property* prop, const core::TimeContext* ctx);

    // Any other global a script can read.
    void set(const char* name, double value);
    void set(const char* name, const core::Value& value);

    // Max Lua instructions per evaluation. Default 200,000: enough for real expressions,
    // low enough that `while true do end` stops rather than hangs.
    void setInstructionBudget(int instructions);

    // Compiled chunks kept; expressions compile once and are reused since per-frame
    // compilation would dominate cost.
    [[nodiscard]] std::size_t cachedChunks() const noexcept;

    // Public so this module's TUs can share it; defined in an internal header that
    // never leaves src/, keeping lua_State behind one door.
    class Impl;

private:
    Sandbox();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ruby::script
