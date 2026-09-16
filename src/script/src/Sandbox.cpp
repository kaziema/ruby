#include "SandboxImpl.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace ruby::script {
namespace {

// Lua's allocator with a memory ceiling; a null return raises Lua's normal OOM error,
// caught by pcall like any other.
void* cappedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* impl = static_cast<Sandbox::Impl*>(ud);
    const std::size_t had = (ptr != nullptr) ? osize : 0;

    if (nsize == 0) {
        impl->memoryUsed -= had;
        std::free(ptr);
        return nullptr;
    }
    if (nsize > had && impl->memoryUsed + (nsize - had) > impl->memoryBudget) {
        impl->outOfMemory = true;
        return nullptr;
    }
    void* made = std::realloc(ptr, nsize);
    if (made != nullptr) {
        impl->memoryUsed = impl->memoryUsed - had + nsize;
    }
    return made;
}

Sandbox::Impl* implOf(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "ruby.impl");
    auto* impl = static_cast<Sandbox::Impl*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return impl;
}

// Runs every `budget` instructions; erroring here is the only way to stop a runaway script.
void countHook(lua_State* L, lua_Debug*) {
    if (Sandbox::Impl* impl = implOf(L); impl != nullptr) {
        impl->exhausted = true;
    }
    luaL_error(L, "expression ran too long");
}

// io, os, package, debug, coroutine are never opened (not opened-then-cleared) — nothing
// to find.
void openSafeLibraries(lua_State* L) {
    static const luaL_Reg kSafe[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* lib = kSafe; lib->func != nullptr; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    // load/dofile would let scripts run new code at runtime; collectgarbage can stall
    // the process without tripping the instruction budget. `_G` is removed too: it names
    // the real global table directly, bypassing the fresh per-run environment that a
    // bare assignment falls into.
    for (const char* name : {"dofile", "loadfile", "load", "loadstring", "require",
                             "collectgarbage", "print", "rawequal", "rawlen",
                             "rawget", "rawset", "_G"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }

    // math.random is nondeterministic across runs/machines; expressions get a seeded
    // generator instead (elsewhere).
    lua_getglobal(L, LUA_MATHLIBNAME);
    for (const char* name : {"random", "randomseed"}) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
}

}  // namespace

Sandbox::Impl::~Impl() {
    if (L != nullptr) {
        lua_close(L);
    }
}

// Compiles once and caches by source. Tries `return (source)` first (bare expressions),
// falling back to raw source (multi-line scripts with their own return) — that order
// avoids double-compiling the common case.
int Sandbox::Impl::chunkFor(const std::string& source, std::string* error) {
    if (const auto it = chunks.find(source); it != chunks.end()) {
        return it->second;
    }

    const std::string wrapped = "return (" + source + ")";
    if (luaL_loadbuffer(L, wrapped.c_str(), wrapped.size(), "=expression") != LUA_OK) {
        lua_pop(L, 1);  // the wrapped form's error is noise if the raw form compiles
        if (luaL_loadbuffer(L, source.c_str(), source.size(), "=expression") != LUA_OK) {
            if (error != nullptr) {
                *error = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1)
                                                        : "could not compile";
            }
            lua_pop(L, 1);
            return LUA_NOREF;
        }
    }
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    chunks.emplace(source, ref);
    return ref;
}

Sandbox::Sandbox() : impl_(std::make_unique<Impl>()) {}
Sandbox::~Sandbox() = default;

std::unique_ptr<Sandbox> Sandbox::create() {
    std::unique_ptr<Sandbox> self(new Sandbox());
    self->impl_->L = lua_newstate(cappedAlloc, self->impl_.get());
    if (self->impl_->L == nullptr) {
        return nullptr;
    }
    lua_State* L = self->impl_->L;

    lua_pushlightuserdata(L, self->impl_.get());
    lua_setfield(L, LUA_REGISTRYINDEX, "ruby.impl");
    lua_pushlightuserdata(L, &self->impl_->inputs);
    lua_setfield(L, LUA_REGISTRYINDEX, "ruby.inputs");

    openSafeLibraries(L);
    installExpressionLibrary(L);
    return self;
}

void Sandbox::setInstructionBudget(int instructions) {
    impl_->budget = instructions > 0 ? instructions : 1;
}

void Sandbox::setInputs(double time, const core::Value& value, std::uint64_t seed) {
    impl_->inputs.time = time;
    impl_->inputs.value = value;
    impl_->inputs.seed = seed;

    // Globals, not functions, because that's what they are in AE — `value + 20` must
    // work as pasted.
    lua_pushnumber(impl_->L, time);
    lua_setglobal(impl_->L, "time");
    pushValue(impl_->L, value);
    lua_setglobal(impl_->L, "value");
}

void Sandbox::setProperty(const core::Property* prop, const core::TimeContext* ctx) {
    impl_->inputs.property = prop;
    impl_->inputs.ctx = ctx;
}

std::size_t Sandbox::cachedChunks() const noexcept { return impl_->chunks.size(); }

void Sandbox::set(const char* name, double value) {
    lua_pushnumber(impl_->L, value);
    lua_setglobal(impl_->L, name);
}

void Sandbox::set(const char* name, const core::Value& value) {
    lua_State* L = impl_->L;
    if (value.count <= 1) {
        lua_pushnumber(L, value.c[0]);
        lua_setglobal(L, name);
        return;
    }
    // Indexed from 1 (Lua convention); AE's 0-based indexing is reconciled in the paste
    // shim, not here.
    lua_createtable(L, value.count, 0);
    for (int i = 0; i < value.count; ++i) {
        lua_pushnumber(L, value.c[static_cast<std::size_t>(i)]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setglobal(L, name);
}

Sandbox::Outcome Sandbox::evaluate(const std::string& source) {
    Outcome out;
    lua_State* L = impl_->L;

    const int ref = impl_->chunkFor(source, &out.error);
    if (ref == LUA_NOREF) {
        return out;
    }

    impl_->exhausted = false;
    impl_->outOfMemory = false;
    lua_sethook(L, countHook, LUA_MASKCOUNT, impl_->budget);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);

    // Fresh environment per run: reads fall through to real globals, writes land in a
    // throwaway table. Without this, a global write in one expression (e.g. redefining
    // wiggle) would corrupt every other expression in the project.
    lua_newtable(L);                       // env
    lua_newtable(L);                       // metatable
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");

    // Hidden, or `getmetatable(_ENV).__index` exposes the real global table and undoes
    // the sandboxing above.
    lua_pushliteral(L, "environment");
    lua_setfield(L, -2, "__metatable");

    lua_setmetatable(L, -2);
    lua_setupvalue(L, -2, 1);              // _ENV is a main chunk's first upvalue

    const int status = lua_pcall(L, 0, 1, 0);

    // Cleared even on error — a stale hook would fire on the next evaluation and kill an
    // unrelated expression.
    lua_sethook(L, nullptr, 0, 0);

    if (status != LUA_OK) {
        out.exhausted = impl_->exhausted || impl_->outOfMemory;
        out.error = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "failed";
        lua_pop(L, 1);
        return out;
    }

    if (readValue(L, -1, out.value)) {
        // Non-finite results are refused, not propagated — NaN would silently spread
        // and produce a layer that just doesn't draw.
        bool finite = true;
        for (int i = 0; i < out.value.count; ++i) {
            if (!std::isfinite(out.value.c[static_cast<std::size_t>(i)])) {
                finite = false;
            }
        }
        if (finite) {
            out.ok = true;
        } else {
            out.error = "expression produced a value that is not a finite number";
        }
    } else {
        out.error = "expression did not return a number or a vector";
    }
    lua_pop(L, 1);
    return out;
}

}  // namespace ruby::script
