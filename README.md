# Ruby

A layer-based compositor and motion tool for people who make edits.

Working title. Cross-platform C++, Qt 6, GPU compositing.

---

## What this is

After Effects is the tool the edit community actually uses, and it fights them the entire
way. It is expensive, it is slow to preview, its learning curve is vertical, and the whole
preset economy it supports is built on sand: a `.ffx` file is a list of references to
plugins, so the moment you do not own Sapphire or Twixtor, the preset you paid for opens as
a dead layer.

We took apart a real, commercially sold editing pack to check that. Of its 26 presets, 23
require paid third-party plugins to function at all. That is the problem.

This is an editor built for that audience specifically, with three things AE structurally
cannot offer.

**Presets always work.** Every effect ships with the app. There is no third-party plugin
layer, so there is no missing-plugin failure mode. A preset is one file, you drag it in, it
works, on every install, forever. The only way it can fail is if the pack is newer than your
app, and the fix for that is "update the app," not "go buy a $600 plugin suite."

**The effects are meant to be plugin grade, colour especially.** That promise above is
worthless if the built-ins are worse than what people are currently paying for. Of the 9 colour
presets in that pack we took apart, 8 needed paid plugins, and they all followed the same
recipe with Magic Bullet Looks in the middle of it. So the target is specific: one Look effect
with a properly ordered internal chain, float linear-light processing throughout, real tonal
control rather than brightness and contrast sliders, and film character like halation, bloom
and grain. Compositing already happens in linear light, which is the single biggest reason
stock effects tend to look worse than the paid ones.

**Presets are resolution and tempo independent.** Every parameter declares its unit. A blur
radius stored as a percentage of the frame diagonal survives being moved from 1920x1080 to
1080x1920. A transition stored in beats survives being moved from a 90 BPM song to a 174 BPM
one. AE stores pixels and frames, which is why pack authors ship separate 30fps and 60fps
versions of everything.

**The beat is a first-class object.** The project analyzes your track once on import and
owns a real beat grid with downbeats. Keyframes quantize to it, effects can be driven by it
directly, and cuts land on it. Not markers you place by hand.

## Current Status

**Pre-alpha, but not empty.** There's a real GPU compositor behind the window now, not just
a themed shell. Treat anything below not listed as done as aspirational, and treat the
roadmap in `NOTEBOOK.md` as intent, not a promise.

Done so far:
- Real GPU render path (Dawn/WebGPU), linear-light compositing in RGBA16Float, premultiplied
  alpha
- 8 built-in effects, all running on the real adapter: Grade, Lift Gamma Gain, Gaussian Blur,
  Directional Blur, Chromatic Aberration, Glow, Vignette, Posterize
- Direct manipulation in the viewer: select, move, rotate, and resize a layer by dragging it,
  not just by typing numbers into the inspector
- Multi-layer selection, align, and distribute
- Timeline with keyframes, effect stacks, and a moveable-panel workspace (drag panels
  between tabs, like a real dock)
- A render graph and a RAM-tier preview cache, so scrubbing doesn't re-render effects that
  didn't change
- Video and audio import and decode (FFmpeg), a real-time audio mixer, and a per-layer
  keyframeable Audio Level control
- Project save/load (`.rbypr`, JSON) with a schema + migration harness, so old projects
  keep opening as the app changes
- 31 automated tests, `ctest`-driven, covering the document model, the render graph, the
  mixer, and the viewer's hit-testing math

Still to do:
- **Beat detection and the beat map itself.** The private tree (`src/beat`) exists as an
  interface and a stub; the actual detector isn't wired in yet. This is the pillar feature
  and it isn't built.
- **Render and export.** There is no way to get a file out of Ruby yet. The compositor can
  put pixels on screen; it cannot write them to disk. GPU texture readback doesn't exist
  either, which also blocks the preview cache's disk tier.
- **Masking, as a framework concern**, transitions, shape and adjustment layers. Timeline
  switches for some of these already exist and are inert.
- **The preset ecosystem.** Deliberately last. A preset format exists (`schemas/preset/`);
  nothing authors or ships one yet.
- Windows and Linux. macOS is the only platform built and tested.

## Built on

- **[Qt 6](https://www.qt.io/)** (LGPL, dynamically linked) — every panel is custom-painted,
  not stock widgets, but Qt owns the window and the event loop.
- **[Dawn](https://dawn.googlesource.com/dawn)** — the WebGPU implementation behind Ruby's
  GPU device abstraction.
- **[FFmpeg](https://ffmpeg.org/)** — video and audio decode. Dev builds link Homebrew's GPL
  build; it has to be rebuilt LGPL (no `--enable-gpl`, no libx264) before this ships.
- **[miniaudio](https://miniaudio.io/)** — the audio device backend.
- **[nlohmann/json](https://github.com/nlohmann/json)** — project files and effect schemas.
- **[Lua 5.4](https://www.lua.org/)** (MIT) — the scripting and expression sandbox.
- **[beat_this_cpp](https://github.com/CPJKU/beat_this)** (MIT, including model weights) —
  beat detection, used only in the private `src/beat` tree, not yet wired up.

## Building

Requires a C++20 compiler, CMake 3.24+, Ninja, and Qt 6.

macOS is the only platform currently tested. Windows and Linux are intended targets and are
not wired up yet.

```sh
brew install qt cmake ninja

cmake -S . -B build -G Ninja
cmake --build build
```

Run it:

```sh
open build/src/app/ruby.app          # macOS
```

Run the tests:

```sh
ctest --test-dir build --output-on-failure
```

CMake looks for Homebrew's Qt automatically. If yours lives somewhere else, point at it:

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/qt
```

## Layout

```
src/core      document model: project/composition/layer/property, transforms, units,
              effect and parameter identity, migration
src/engine    effect registry, GPU compositor, render graph, preview cache
src/gpu       GPU device abstraction, backed by Dawn/WebGPU
src/io        project save/load, effect schema I/O, undo history, media pool
src/media     video/audio decode (FFmpeg), waveform peaks
src/audio     real-time audio mixer
src/script    Lua sandbox, expressions, After Effects import
src/ui        Qt interface: timeline, inspector, viewer, panels, theme
src/app       entry point
src/beat      beat detection interface (private implementation, not public)
tests/        31 tests, run via ctest
schemas/      effect and preset JSON schemas
```

Two decisions in `src/core` are worth reading before touching anything, because they are
permanent contracts rather than implementation details.

`Units.h` is why presets are portable. Time is stored in beats or seconds, never frames
unless an effect explicitly opts in, because a frame is not a unit of time, it is time
divided by whatever framerate a project happens to use.

`Identity.h` is why presets keep working across versions. Effect IDs and parameter keys are
immortal. Display order and labels are not. Deleting a parameter retires its key forever.
Changing a default requires pinning the old one so existing projects do not silently shift.
These rules are enforced by `validate()` and covered by tests, so breaking one fails the
build instead of quietly corrupting every preset ever made. This is adapted from After
Effects' own disk-ID system, which is the one part of AE's architecture that is unambiguously
correct.

## What is not in this repo

This is open core. Three things ship only in the paid build:

- **Render and export**
- **Beat detection and beat editing**
- **The handcrafted preset packs**

The entire effects library is here, deliberately. A preset is worthless if the effect under
it is missing, and "presets always work" is the whole promise, so withholding effects would
break that promise on our own repo.

A build from this tree will be a real compositor with a complete effect set. You will be able
to cut by hand, build your own presets against exactly the same effects we use, and preview
in real time. You will not be able to detect a beat, cut to one, render a file, or get our
packs.

See [LICENSE-FAQ.md](LICENSE-FAQ.md) for the plain-language version.

## License

[Business Source License 1.1](LICENSE). This is **source available**, not open source, and
the distinction matters enough that we are not going to blur it.

Use it for anything, including commercial work, and sell everything you make with it. Do not
take this code and ship a competing editor. Each released version converts to GPL v3 four
years after its release.

The bundled preset packs are not in this repo and are not covered by this license.

## Contributing

Sign the [CLA](CLA.md) in your first pull request. It is one page and it exists because
contributed code has to be usable in the paid build. Without it, every contributor keeps
separate copyright over their patch and the project cannot ship.

Contributions are welcome, but the project is at the stage where most of it is still being
decided. Open an issue before writing anything substantial.
