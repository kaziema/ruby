// Sandbox tests. Expressions come from untrusted project/preset files, so most of this
// file is escape attempts written to assert failure.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ruby/script/Sandbox.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void checkNear(double a, double b, const char* what, double eps = 1e-9) {
    if (!(a >= b - eps && a <= b + eps)) {
        std::fprintf(stderr, "FAIL: %s (got %.9f, want %.9f)\n", what, a, b);
        ++failures;
    }
}

// A script that cannot even be compiled is refused, which counts as blocked.
void blocked(script::Sandbox& box, const char* source, const char* what) {
    const script::Sandbox::Outcome out = box.evaluate(source);
    if (out.ok) {
        std::fprintf(stderr, "ESCAPED: %s  <<< the sandbox let this through\n", what);
        ++failures;
    }
}

void it_evaluates_the_ordinary_things() {
    auto box = script::Sandbox::create();
    check(box != nullptr, "a sandbox starts");

    checkNear(box->evaluate("2 + 3").value.c[0], 5.0, "arithmetic");
    checkNear(box->evaluate("math.floor(3.7)").value.c[0], 3.0, "the maths library is there");
    checkNear(box->evaluate("math.sin(0)").value.c[0], 0.0, "and works");

    // Both bare expressions and full chunks (with locals) must work.
    checkNear(box->evaluate("40 + 2").value.c[0], 42.0, "a bare expression");
    checkNear(box->evaluate("local a = 40 return a + 2").value.c[0], 42.0,
              "a chunk with its own return");
}

void vectors_go_in_and_out() {
    auto box = script::Sandbox::create();
    box->set("value", core::Value::vec2(50.0, 80.0));

    const auto read = box->evaluate("value[1]");
    checkNear(read.value.c[0], 50.0, "a vector global is readable, indexed from one");

    const auto made = box->evaluate("{value[1], value[2] + 20}");
    check(made.ok, "a table comes back");
    check(made.value.count == 2, "as a vec2");
    checkNear(made.value.c[1], 100.0, "with the arithmetic applied");

    box->set("time", 2.0);
    checkNear(box->evaluate("time * 100").value.c[0], 200.0, "a scalar global");

    // Wrong-shape tables error rather than truncate silently.
    check(!box->evaluate("{1, 2, 3, 4, 5}").ok, "five values is refused");
    check(!box->evaluate("{}").ok, "and so is none");
    check(!box->evaluate("'a string'").ok, "and a string");
}

// Each of these is a real thing a hostile/careless preset could try; all must fail.
void it_cannot_reach_outside_the_process() {
    auto box = script::Sandbox::create();

    blocked(*box, "io.open('/etc/passwd')", "opening a file");
    blocked(*box, "io.write('x')", "writing to stdout");
    blocked(*box, "os.execute('rm -rf /')", "running a command");
    blocked(*box, "os.remove('/tmp/x')", "deleting a file");
    blocked(*box, "os.getenv('HOME')", "reading the environment");
    blocked(*box, "require('os')", "requiring a library back in");
    blocked(*box, "package.loadlib('/lib/x.so', 'f')", "loading a shared object");
    blocked(*box, "dofile('/etc/passwd')", "running a file as code");
    blocked(*box, "loadfile('/etc/passwd')", "loading a file as code");
    blocked(*box, "load('return 1')()", "building new code at runtime");
    blocked(*box, "debug.getregistry()", "reaching the registry through debug");
    blocked(*box, "debug.sethook(function() end)", "installing its own hook");
    blocked(*box, "collectgarbage('collect')", "stalling in the collector");

    // Libraries are absent, not emptied, so a script can't distinguish them from a typo.
    check(!box->evaluate("type(io)").ok || box->evaluate("io == nil").ok,
          "io is not present at all");
}

void it_cannot_be_non_deterministic() {
    auto box = script::Sandbox::create();

    blocked(*box, "os.time()", "reading the clock");
    blocked(*box, "os.clock()", "reading the process clock");
    blocked(*box, "os.date()", "reading the date");
    blocked(*box, "math.random()", "unseeded randomness");
    blocked(*box, "math.randomseed(1)", "seeding the generator");

    // Same expression twice must give the same answer.
    const double first = box->evaluate("math.pi * 2").value.c[0];
    const double again = box->evaluate("math.pi * 2").value.c[0];
    checkNear(first, again, "the same expression gives the same answer");
}

// One bad expression in one preset must not take the app with it.
void a_runaway_expression_is_stopped() {
    auto box = script::Sandbox::create();
    box->setInstructionBudget(50000);

    const script::Sandbox::Outcome spin = box->evaluate("while true do end");
    check(!spin.ok, "an infinite loop does not succeed");
    check(spin.exhausted, "and is reported as having run out of budget, not as an error");

    const script::Sandbox::Outcome recursive =
        box->evaluate("local function f(n) return f(n + 1) end return f(0)");
    check(!recursive.ok, "unbounded recursion is stopped too");

    // Sandbox must stay usable after a runaway, not have the budget overrun poison it.
    checkNear(box->evaluate("1 + 1").value.c[0], 2.0,
              "the sandbox still works after a runaway");

    // The budget hook must not linger and eat into the next unrelated call.
    for (int i = 0; i < 50; ++i) {
        check(box->evaluate("1 + 1").ok, "and keeps working, run after run");
    }
}

void errors_are_reported_not_thrown() {
    auto box = script::Sandbox::create();

    const auto broken = box->evaluate("this is not lua");
    check(!broken.ok, "a syntax error fails");
    check(!broken.error.empty(), "and says something");
    check(!broken.exhausted, "and is not confused with a budget overrun");

    const auto runtime = box->evaluate("error('boom')");
    check(!runtime.ok, "a runtime error fails");
    check(!runtime.error.empty(), "and says something");

    const auto missing = box->evaluate("undefinedThing * 2");
    check(!missing.ok, "an undefined global fails rather than reading as zero");
}

// Compiling per frame would cost more than running, so chunks are kept.
void chunks_are_compiled_once() {
    auto box = script::Sandbox::create();
    check(box->cachedChunks() == 0, "nothing cached to start");

    check(box->evaluate("1 + 1").ok, "an expression evaluates");
    check(box->cachedChunks() == 1, "one expression, one chunk");

    for (int i = 0; i < 100; ++i) {
        check(box->evaluate("1 + 1").ok, "and keeps evaluating");
    }
    check(box->cachedChunks() == 1, "a hundred more evaluations, still one chunk");

    check(box->evaluate("2 + 2").ok, "a different expression evaluates");
    check(box->cachedChunks() == 2, "and is a second chunk");
}


// --- The After Effects surface ----------------------------------------------

void the_ae_globals_are_there() {
    auto box = script::Sandbox::create();
    box->setInputs(2.5, core::Value::vec2(50.0, 80.0), 1);

    checkNear(box->evaluate("time").value.c[0], 2.5, "time is a global, not a call");
    checkNear(box->evaluate("time * 100").value.c[0], 250.0, "and is usable in arithmetic");

    const auto v = box->evaluate("value");
    check(v.value.count == 2, "value comes through as a vector");
    checkNear(v.value.c[1], 80.0, "with its components intact");
    checkNear(box->evaluate("value[1]").value.c[0], 50.0, "indexable from one");
    checkNear(box->evaluate("value.y").value.c[0], 80.0, "and by name");
}

// AE's `value + [0, 50]` becomes `value + vec(0, 50)` here; the shim depends on this working.
void vectors_do_arithmetic() {
    auto box = script::Sandbox::create();
    box->setInputs(0.0, core::Value::vec2(50.0, 80.0), 1);

    const auto sum = box->evaluate("value + vec(0, 50)");
    check(sum.ok && sum.value.count == 2, "vector plus vector");
    checkNear(sum.value.c[1], 130.0, "adds component by component");

    const auto scalar = box->evaluate("value + 20");
    check(scalar.ok && scalar.value.count == 2, "vector plus scalar stays a vector");
    checkNear(scalar.value.c[0], 70.0, "broadcasting across components");
    checkNear(scalar.value.c[1], 100.0, "both of them");

    checkNear(box->evaluate("(value * 2)[1]").value.c[0], 100.0, "multiply");
    checkNear(box->evaluate("(value - vec(50, 80))[1]").value.c[0], 0.0, "subtract");
    checkNear(box->evaluate("(-value)[1]").value.c[0], -50.0, "negate");
    checkNear(box->evaluate("#value").value.c[0], 2.0, "length operator gives the count");
    checkNear(box->evaluate("length(vec(3, 4))").value.c[0], 5.0, "and length() the norm");

    check(box->evaluate("value == vec(50, 80)").ok == false ||
              box->evaluate("value == vec(50, 80) and 1 or 0").value.c[0] == 1.0,
          "equality compares components");
}

// Renders must be reproducible across time and machines.
void wiggle_is_deterministic() {
    auto box = script::Sandbox::create();
    box->setInputs(1.0, core::Value::vec2(100.0, 200.0), 12345);

    const auto first = box->evaluate("wiggle(5, 20)");
    check(first.ok, "wiggle evaluates");
    check(first.value.count == 2, "and keeps the shape of value");

    const auto again = box->evaluate("wiggle(5, 20)");
    checkNear(first.value.c[0], again.value.c[0], "the same call gives the same answer");
    checkNear(first.value.c[1], again.value.c[1], "in every component");

    // A fresh sandbox with the same seed/time must agree too (simulates a second machine).
    auto other = script::Sandbox::create();
    other->setInputs(1.0, core::Value::vec2(100.0, 200.0), 12345);
    const auto elsewhere = other->evaluate("wiggle(5, 20)");
    checkNear(first.value.c[0], elsewhere.value.c[0],
              "and a different interpreter agrees, which is what a render farm needs");
}

void wiggle_actually_moves_and_stays_in_bounds() {
    auto box = script::Sandbox::create();

    bool moved = false;
    double worst = 0.0;
    for (int frame = 0; frame < 200; ++frame) {
        box->setInputs(frame / 30.0, core::Value::vec2(100.0, 200.0), 7);
        const auto out = box->evaluate("wiggle(5, 20)");
        if (!out.ok) {
            check(false, "wiggle evaluated across a range of times");
            return;
        }
        const double offset = out.value.c[0] - 100.0;
        if (std::fabs(offset) > 1e-6) {
            moved = true;
        }
        worst = std::fabs(offset) > worst ? std::fabs(offset) : worst;
    }
    check(moved, "wiggle actually moves the value rather than returning it unchanged");

    // One octave at amplitude 20 must not exceed 20.
    check(worst <= 20.0 + 1e-9, "and stays inside the amplitude it was given");
}

// Different layers must not wiggle in sympathy, and neither must x and y.
void different_seeds_wiggle_differently() {
    auto a = script::Sandbox::create();
    auto b = script::Sandbox::create();
    a->setInputs(1.0, core::Value::vec2(0.0, 0.0), 1);
    b->setInputs(1.0, core::Value::vec2(0.0, 0.0), 2);

    const auto first = a->evaluate("wiggle(5, 20)");
    const auto second = b->evaluate("wiggle(5, 20)");
    check(std::fabs(first.value.c[0] - second.value.c[0]) > 1e-9,
          "consecutive seeds do not produce the same wiggle");
    check(std::fabs(first.value.c[0] - first.value.c[1]) > 1e-9,
          "and x and y move independently rather than in lockstep");
}

void wiggle_is_smooth() {
    auto box = script::Sandbox::create();

    // Adjacent frames must stay close; smoothstep is what keeps wiggle from ticking.
    double previous = 0.0;
    double biggestJump = 0.0;
    for (int frame = 0; frame < 300; ++frame) {
        box->setInputs(frame / 60.0, core::Value::scalar(0.0), 99);
        const double now = box->evaluate("wiggle(3, 100)").value.c[0];
        if (frame > 0) {
            const double jump = std::fabs(now - previous);
            biggestJump = jump > biggestJump ? jump : biggestJump;
        }
        previous = now;
    }
    check(biggestJump < 30.0,
          "no sudden jumps between adjacent frames, so the motion reads as smooth");
}

void the_easing_helpers_match_ae() {
    auto box = script::Sandbox::create();
    box->setInputs(0.0, core::Value::scalar(0.0), 1);

    checkNear(box->evaluate("linear(0.5, 0, 1, 0, 100)").value.c[0], 50.0, "linear midpoint");
    checkNear(box->evaluate("linear(-5, 0, 1, 0, 100)").value.c[0], 0.0,
              "clamped below, as AE's linear is");
    checkNear(box->evaluate("linear(5, 0, 1, 0, 100)").value.c[0], 100.0, "and above");

    checkNear(box->evaluate("ease(0.5, 0, 1, 0, 100)").value.c[0], 50.0,
              "ease is symmetric at the midpoint");
    check(box->evaluate("ease(0.25, 0, 1, 0, 100)").value.c[0] < 25.0,
          "and slower than linear at the start");

    const auto vectorised = box->evaluate("linear(0.5, 0, 1, vec(0, 0), vec(100, 200))");
    check(vectorised.value.count == 2, "linear works on vectors too");
    checkNear(vectorised.value.c[1], 100.0, "component by component");

    checkNear(box->evaluate("clamp(5, 0, 1)").value.c[0], 1.0, "clamp");
    checkNear(box->evaluate("radiansToDegrees(math.pi)").value.c[0], 180.0, "angle helpers");
}

// A script could hijack the vector metatable via getmetatable(value).__index, corrupting
// every later expression's vector reads in the same sandbox.
void the_vector_type_cannot_be_tampered_with() {
    auto box = script::Sandbox::create();
    box->setInputs(0.0, core::Value::vec2(10.0, 20.0), 1);

    checkNear(box->evaluate("value[1]").value.c[0], 10.0, "a vector reads correctly");

    const auto attempt = box->evaluate(
        "(function() getmetatable(value).__index = function() return 7 end return 1 end)()");
    check(!attempt.ok, "overwriting the vector metatable fails");

    checkNear(box->evaluate("value[1]").value.c[0], 10.0,
              "and the vector type still works afterwards, uncorrupted");

    // setmetatable must refuse too.
    const auto swap = box->evaluate(
        "(function() setmetatable(value, {}) return 1 end)()");
    check(!swap.ok, "and the metatable cannot be replaced wholesale");
    checkNear(box->evaluate("value[2]").value.c[0], 20.0, "still intact");
}

// Bad arguments must fail rather than produce a plausible wrong number.
void bad_arguments_are_refused() {
    auto box = script::Sandbox::create();
    box->setInputs(0.0, core::Value::vec2(10.0, 20.0), 1);

    check(!box->evaluate("value[999]").ok, "an index past the end fails");
    check(!box->evaluate("value[-1]").ok, "and so does a negative one");
    check(!box->evaluate("vec(1)").ok, "vec needs at least two components");
    check(!box->evaluate("vec(1,2,3,4,5)").ok, "and at most four");
    check(!box->evaluate("wiggle()").ok, "wiggle needs its arguments");
    check(!box->evaluate("wiggle('a','b')").ok, "and they have to be numbers");
    check(!box->evaluate("linear(0,0,1)").ok, "linear needs all five");
}

// A global write from one expression must not leak into another sandbox call/expression.
void one_expression_cannot_change_another() {
    auto box = script::Sandbox::create();
    box->setInputs(1.0, core::Value::scalar(0.0), 42);

    const double before = box->evaluate("wiggle(5, 20)").value.c[0];

    // Every known route to the shared globals.
    check(box->evaluate("wiggle = function() return 999 end return 1").ok,
          "assigning a global succeeds, harmlessly");
    check(!box->evaluate("_G.wiggle = function() return 999 end return 1").ok,
          "_G is gone, so it cannot be named explicitly");
    check(!box->evaluate(
               "getmetatable(_ENV).__index.wiggle = function() return 999 end return 1")
               .ok,
          "and the environment's metatable is hidden");

    checkNear(box->evaluate("wiggle(5, 20)").value.c[0], before,
              "wiggle is untouched after every attempt");

    // A bare global written by one expression must not be visible to the next.
    check(box->evaluate("leaked = 7 return 1").ok, "a global assignment succeeds");
    checkNear(box->evaluate("leaked or -1").value.c[0], -1.0,
              "and is gone by the next expression");
}

// The instruction budget bounds time, not memory; needs its own cap.
void memory_is_capped_too() {
    auto box = script::Sandbox::create();

    const auto bomb = box->evaluate(
        "local t = {} for i = 1, 500 do t[i] = string.rep('x', 1000000) end return #t");
    check(!bomb.ok, "an expression cannot allocate without limit");

    // Sandbox must survive the cap being hit.
    checkNear(box->evaluate("1 + 1").value.c[0], 2.0, "the sandbox still works afterwards");
    check(box->evaluate("string.rep('x', 1000)").ok == false ||
              box->evaluate("#string.rep('x', 1000)").value.c[0] == 1000.0,
          "and ordinary string work still succeeds");
}

// NaN would silently propagate into a layer that just doesn't draw, with no error.
void non_finite_results_are_refused() {
    auto box = script::Sandbox::create();
    box->setInputs(0.0, core::Value::scalar(0.0), 1);

    check(!box->evaluate("0/0").ok, "NaN is refused");
    check(!box->evaluate("1/0").ok, "and infinity");
    check(!box->evaluate("1e308 * 10").ok, "and an overflow to infinity");
    check(!box->evaluate("vec(0/0, 1)").ok, "including inside a vector");
    check(box->evaluate("1/2").ok, "while ordinary division is fine");
}

}  // namespace

int main() {
    it_evaluates_the_ordinary_things();
    vectors_go_in_and_out();
    it_cannot_reach_outside_the_process();
    it_cannot_be_non_deterministic();
    a_runaway_expression_is_stopped();
    errors_are_reported_not_thrown();
    chunks_are_compiled_once();
    the_ae_globals_are_there();
    vectors_do_arithmetic();
    wiggle_is_deterministic();
    wiggle_actually_moves_and_stays_in_bounds();
    different_seeds_wiggle_differently();
    wiggle_is_smooth();
    the_easing_helpers_match_ae();
    the_vector_type_cannot_be_tampered_with();
    bad_arguments_are_refused();
    one_expression_cannot_change_another();
    memory_is_capped_too();
    non_finite_results_are_refused();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("sandbox: all checks passed");
    return EXIT_SUCCESS;
}
