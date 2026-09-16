#pragma once

// Internal to the script module — lua_State must not leak out, or this stops being the
// only door into the sandbox.

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdint>
#include <map>
#include <string>

#include "ruby/core/Document.h"
#include "ruby/script/Sandbox.h"

namespace ruby::script {

// What an expression is being evaluated *for*. Set before each run.
struct Inputs {
    double time = 0.0;
    core::Value value;

    // Stable per layer/property, constant across frames — makes wiggle deterministic
    // and distinct per layer.
    std::uint64_t seed = 0;

    // Borrowed, valid only for one evaluate() call; nothing holds on to them.
    const core::Property* property = nullptr;
    const core::TimeContext* ctx = nullptr;
};

class Sandbox::Impl {
public:
    lua_State* L = nullptr;
    int budget = 200000;
    bool exhausted = false;

    // Bytes Lua may hold. The instruction budget bounds time only; a few instructions
    // can still allocate gigabytes via string ops.
    std::size_t memoryBudget = 64ULL * 1024 * 1024;
    std::size_t memoryUsed = 0;
    bool outOfMemory = false;
    Inputs inputs;
    std::map<std::string, int> chunks;  // source -> registry reference

    ~Impl();

    int chunkFor(const std::string& source, std::string* error);
};

// Vector metatable name; tables carrying it get arithmetic (value + vec(0, 50), etc.).
inline constexpr const char* kVecMeta = "ruby.vec";

// Pushes a core::Value as a number or as a vector table with the metatable attached.
void pushValue(lua_State* L, const core::Value& value);

// Reads a number or vector table back off the stack. Returns false for anything else.
bool readValue(lua_State* L, int index, core::Value& out);

// Registers time, value, wiggle, the easing helpers and the vector type.
void installExpressionLibrary(lua_State* L);

}  // namespace ruby::script
