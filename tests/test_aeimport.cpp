// AE paste shim: the line between "converted" and "refused". A silently wrong number is
// worse than a refusal, so warn cases matter as much as convert cases.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "ruby/script/AeImport.h"
#include "ruby/script/LuaHost.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void converts(const char* from, const char* to) {
    const script::Conversion c = script::convertFromAfterEffects(from);
    if (c.lua != to) {
        std::fprintf(stderr, "FAIL: %s\n   got  %s\n   want %s\n", from, c.lua.c_str(), to);
        ++failures;
    }
}

void warns(const char* source, const char* what) {
    const script::Conversion c = script::convertFromAfterEffects(source);
    if (c.clean()) {
        std::fprintf(stderr, "FAIL: %s should have warned about %s\n", source, what);
        ++failures;
    }
}

// The lines people actually paste.
void the_common_shapes_convert() {
    converts("wiggle(30, 10)", "wiggle(30, 10)");
    converts("time * 100", "time * 100");
    converts("value + [0, 50]", "value + vec(0, 50)");
    converts("[value[0], value[1] + 20]", "vec(value[1], value[2] + 20)");
    converts("var amp = 50; wiggle(3, amp)", "local amp = 50; wiggle(3, amp)");
    converts("Math.sin(time)", "math.sin(time)");
    converts("// a comment", "-- a comment");
    converts("a != b", "a ~= b");
    converts("a && b", "a and b");
    converts("a || b", "a or b");
    converts("a === b", "a == b");
}

// The distinction a regular expression cannot make: an index versus a literal.
void indexing_is_told_apart_from_literals() {
    converts("value[0]", "value[1]");
    converts("[0, 50]", "vec(0, 50)");
    converts("foo(bar)[0]", "foo(bar)[1]");
    // Nested: literal contents must not shift, only the trailing index.
    converts("[[0, 1], [2, 3]][0]", "vec(vec(0, 1), vec(2, 3))[1]");
    converts("x + [1, 2]", "x + vec(1, 2)");
}

// Strings must pass through untouched, or loopOut('pingpong') breaks on paste.
void strings_are_left_alone() {
    converts("loopOut('pingpong')", "loopOut('pingpong')");
    converts("f(\"var Math. [0,1]\")", "f(\"var Math. [0,1]\")");
}

// Each of these could be guessed at, but a wrong guess is worse than refusing.
void the_things_it_cannot_do_are_reported() {
    warns("a ? b : c", "a ternary");
    warns("thisComp.layer('Null 1')", "thisComp");
    warns("if (time > 2) { value }", "control flow");
    warns("value[i]", "a computed index");
    warns("function f() {}", "a function");
}

void it_says_what_it_changed() {
    const script::Conversion c =
        script::convertFromAfterEffects("var a = [0, 1]; Math.max(a[0], 2)");
    check(!c.notes.empty(), "a conversion explains itself");
    check(c.clean(), "and this one had nothing it could not handle");
}

// Detection only; running conversion on already-Lua code would break it.
void it_recognises_what_needs_converting() {
    check(script::looksLikeAfterEffects("value + [0, 50]"), "an array literal is a tell");
    check(script::looksLikeAfterEffects("var x = 1"), "and var");
    check(script::looksLikeAfterEffects("Math.sin(t)"), "and Math.");

    check(!script::looksLikeAfterEffects("wiggle(30, 10)"),
          "a line valid in both languages is not flagged");
    check(!script::looksLikeAfterEffects("value + vec(0, 50)"),
          "and neither is Lua that is already converted");
    check(!script::looksLikeAfterEffects("value[1]"),
          "an ordinary index is not an array literal");
}

void converted_expressions_evaluate() {
    auto host = script::LuaHost::create();
    script::ScopedHost installed(host.get());

    core::Project project;
    core::Composition& comp = project.addComposition("c", 1000, 1000, 30.0, 10.0);
    const core::LayerId id = project.addLayer(comp, "l", core::LayerKind::Solid).id;

    core::Property* position = comp.find(id)->find("position");
    position->staticValue = core::Value::vec2(50.0, 40.0);

    const script::Conversion c = script::convertFromAfterEffects("value + [0, 10]");
    check(c.clean(), "converts cleanly");
    position->expression = c.lua;

    const core::Value v =
        core::evaluate(*comp.find(id), *position, 0.0, comp.timeContext());
    check(v.count == 2, "and the result is a vec2");
    check(v.c[1] > 49.9 && v.c[1] < 50.1, "with the AE arithmetic applied");

    // And the index shift is right: AE's value[1] is y, which is Lua's value[2].
    const script::Conversion indexed = script::convertFromAfterEffects("[value[1], value[0]]");
    position->expression = indexed.lua;
    const core::Value swapped =
        core::evaluate(*comp.find(id), *position, 0.0, comp.timeContext());
    check(swapped.c[0] > 39.9 && swapped.c[0] < 40.1,
          "AE's value[1] is y and lands where y should be after conversion");
}

}  // namespace

int main() {
    the_common_shapes_convert();
    indexing_is_told_apart_from_literals();
    strings_are_left_alone();
    the_things_it_cannot_do_are_reported();
    it_says_what_it_changed();
    it_recognises_what_needs_converting();
    converted_expressions_evaluate();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("aeimport: all checks passed");
    return EXIT_SUCCESS;
}
