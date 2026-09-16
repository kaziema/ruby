// Playback clock rules. All pure logic, no timer, which is the point of separating it.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ruby/engine/Transport.h"

using namespace ruby::engine;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void checkNear(double a, double b, const char* what, double eps = 1e-9) {
    if (std::fabs(a - b) > eps) {
        std::fprintf(stderr, "FAIL: %s (got %.9f, want %.9f)\n", what, a, b);
        ++failures;
    }
}

Transport make(double duration = 12.0, double fps = 30.0) {
    Transport t;
    t.setDuration(duration);
    t.setFrameRate(fps);
    return t;
}

void a_stopped_transport_does_not_move() {
    Transport t = make();
    t.setTime(2.0);
    check(!t.advance(1.0), "advancing while stopped reports no frame change");
    checkNear(t.time(), 2.0, "and does not move time");
}

// Time advances by wall clock, not tick count: a slow frame drops a frame, not slows down.
void time_follows_the_wall_clock_not_the_tick_count() {
    Transport t = make();
    t.play();

    t.advance(0.5);
    checkNear(t.time(), 0.5, "a half second of wall clock is a half second of timeline");

    // One long stall, as if a decode blocked. Time still lands where it should.
    t.advance(2.0);
    checkNear(t.time(), 2.5, "a stalled frame skips ahead rather than playing slowly");
}

// Repainting mid-frame is wasted work, so advance only reports true at a boundary.
void frame_changes_are_reported_only_at_boundaries() {
    Transport t = make(12.0, 30.0);  // a frame is 1/30s
    t.play();
    t.setTime(0.0);

    check(!t.advance(0.01), "a third of a frame is not a new frame");
    check(!t.advance(0.01), "two thirds is still not a new frame");
    check(t.advance(0.02), "crossing the boundary is a new frame");
    check(t.frame() == 1, "and the frame index moved by one");
}

void looping_wraps_by_the_remainder() {
    Transport t = make(10.0, 30.0);
    t.play();
    t.setTime(9.5);
    t.setLooping(true);

    t.advance(1.0);
    checkNear(t.time(), 0.5, "wrapping keeps the overshoot instead of snapping to zero");
    check(t.playing(), "looping keeps playing");
}

void not_looping_stops_at_the_end() {
    Transport t = make(10.0, 30.0);
    t.setLooping(false);
    t.play();
    t.setTime(9.5);

    t.advance(1.0);
    checkNear(t.time(), 10.0, "playback lands exactly on the end");
    check(!t.playing(), "and stops");
}

void playing_from_the_end_restarts() {
    Transport t = make(10.0, 30.0);
    t.setLooping(false);
    t.setTime(10.0);
    t.play();
    checkNear(t.time(), 0.0, "hitting play at the end starts over");
    check(t.playing(), "and is playing");
}

void time_is_clamped_to_the_composition() {
    Transport t = make(10.0, 30.0);
    t.setTime(-5.0);
    checkNear(t.time(), 0.0, "negative time clamps to the start");
    t.setTime(999.0);
    checkNear(t.time(), 10.0, "time past the end clamps to the duration");
}

void a_degenerate_frame_rate_does_not_divide_by_zero() {
    Transport t = make(10.0, 0.0);
    check(t.frameRate() > 0.0, "a zero frame rate falls back to something usable");
    t.play();
    t.advance(0.5);
    check(t.frame() >= 0, "and frame indexing still works");
}

}  // namespace

int main() {
    a_stopped_transport_does_not_move();
    time_follows_the_wall_clock_not_the_tick_count();
    frame_changes_are_reported_only_at_boundaries();
    looping_wraps_by_the_remainder();
    not_looping_stops_at_the_end();
    playing_from_the_end_restarts();
    time_is_clamped_to_the_composition();
    a_degenerate_frame_rate_does_not_divide_by_zero();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("transport: all checks passed");
    return EXIT_SUCCESS;
}
