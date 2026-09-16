// Mixer clock and source-publishing tests. Needs a real audio device; returns 77
// (SKIPPED) without one. mix() expects a pre-zeroed block, same as dataCallback provides.

#include <cstdio>
#include <algorithm>
#include <vector>
#include "ruby/audio/AudioOutput.h"
using namespace ruby;

int main() {
    auto out = audio::AudioOutput::create();
    if (!out) { std::puts("no device"); return 77; }
    std::vector<float> block(512 * 2, 0.0f);
    int bad = 0;

    // No sources: clock must still run.
    out->setSources({});
    out->play(0.0);
    for (int i = 0; i < 10; ++i) { std::fill(block.begin(), block.end(), 0.0f); out->mix(block.data(), 512); }
    const double empty = out->position();
    std::printf("empty mix, 10 x 512 frames: position %.4fs (expect ~0.1067)\n", empty);
    if (empty < 0.10 || empty > 0.11) { std::puts("FAIL: clock frozen with no sources"); ++bad; }

    // With a source: audible, clock unaffected.
    media::AudioBuffer buf;
    buf.sampleRate = 48000; buf.channels = 2; buf.samples.assign(48000 * 2, 0.5f);
    audio::AudioSource s; s.buffer = &buf; s.startSeconds = 0.0; s.endSeconds = 1.0;
    out->setSources({s});
    out->play(0.0);
    double energy = 0.0;
    for (int i = 0; i < 10; ++i) { std::fill(block.begin(), block.end(), 0.0f); out->mix(block.data(), 512); for (float v : block) energy += static_cast<double>(v < 0 ? -v : v); }
    std::printf("with source,  10 x 512 frames: position %.4fs  energy %.0f\n", out->position(), energy);
    if (out->position() < 0.10 || out->position() > 0.11) { std::puts("FAIL: clock wrong with a source"); ++bad; }
    if (energy <= 0) { std::puts("FAIL: silent when it should not be"); ++bad; }

    // Delete the layer mid-playback: silent, clock keeps running.
    out->setSources({});
    const double at = out->position();
    energy = 0.0;
    for (int i = 0; i < 10; ++i) { std::fill(block.begin(), block.end(), 0.0f); out->mix(block.data(), 512); for (float v : block) energy += static_cast<double>(v < 0 ? -v : v); }
    std::printf("after delete, 10 x 512 frames: position %.4fs -> %.4fs  energy %.0f\n", at, out->position(), energy);
    if (out->position() <= at) { std::puts("FAIL: clock stopped after delete"); ++bad; }
    if (energy != 0) { std::puts("FAIL: still audible after delete"); ++bad; }

    std::puts(bad ? "FAILED" : "all good");
    return bad ? 1 : 0;
}
