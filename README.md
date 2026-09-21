# Ruby

<img src="docs/ruby-gem.svg" width="120" alt="Ruby" />

A layer-based compositor and motion tool for video editors.

Cross-platform C++, Qt 6, GPU compositing.

---

## What this is

Ruby is a non-linear editor inspired heavily by After Effects. The majority of short-form AMVs/"edits" 
on social platforms are done in After Effects, due to its
extensive powerhouse features. 

Unfortunately, it has a very intense learning curve, as the software was designed with motion graphics in mind. 
not specifically video editing. 
Also, to maximize its potential, you have to download plugin suites. These suites are ridiculously expensive and, if you are to be a little covert about 
where you source it from, can be dangerous to install cracked patches for.

That's where the idea for Ruby was made. I wanted to make an editor that was as intensive with the powerhouse tools and 
features, such as After Effects, designed specifically for video editors. 

### How the idea formed

After downloading ten different packs for Premiere Pro that had no mention of requiring a host of suite plugins (and then finding out that those plugins still cost the same as they did a decade ago), I decided to research not only how NLEs were built but also how they infrastructure exportable items such as presets (JSON, JavaScript, etc.).  

I took apart 5 real, commercially sold editing packs to check that. Out of their almost 150 individual presets, 148
require paid third-party plugins to function at all, with no notice that they were required. That is no bueno.

## Who this is for and what is being delivered

This is an editor built for that audience specifically, with four things AE structurally
cannot offer:

**Presets always work**. Every effect ships within the app, and third-party presets will require NO third-party plugin suite. A preset is one file, and it
works on every install, forever. The only way it can fail is if the pack is newer than your
app, and the fix for that is "update the app," not "go buy a $600 plugin suite."

**The effects are meant to be plug-in grade, especially in color.** The promise above is
worthless if the built-ins are worse than what people are currently paying for.

**Presets are resolution and tempo independent.** Every parameter declares its unit. A transition stored in beats survives being moved from a 90 BPM song to a 174 BPM
one. AE stores pixels and frames, which is why pack authors ship separate 30 fps and 60 fps.
versions of everything.

**Beats and vocals are first-class objects.** The project analyzes your track once on import and
owns a real beat grid with downbeats. Keyframes quantize to it; effects can be driven by it.
directly, and cuts land on it. 

## Current Status

Pre-alpha 

Done so far:

-  GPU render path (Dawn/WebGPU), linear-light compositing in RGBA16Float, premultiplied alpha

- 8 built-in effects running on a real adapter: Grade, Lift Gamma Gain, Gaussian Blur, Directional Blur, Chromatic Aberration, Glow, Vignette, Posterize

- Direct manipulation in the viewer: select, move, rotate, and resize a layer by dragging it

- Multi-layer selection, align, and distribute

- Timeline with keyframes, effect stacks, and a moveable-panel workspace (drag panels between tabs)

- A render graph and a RAM-tier preview cache

- Video and audio import and decode (FFmpeg), a real-time audio mixer, and a per-layer keyframeable audio level control

- Project save/load (`.rbypr`, JSON) with a schema + migration harness

- 31 automated tests, `ctest`-driven, covering the document model, the render graph, the mixer, and the viewer's hit-testing math

Still to do:

- Beat detection and the beat map itself.

- Render and export. 

- Masking, as a framework concern

- The preset ecosystem. 

- Windows and Linux. macOS is the only platform built and tested.

## Built on

- [Qt 6](https://www.qt.io/) (LGPL, dynamically linked)—every panel is custom-painted,

not stock widgets, but Qt owns the window and the event loop.

- [Dawn](https://dawn.googlesource.com/dawn) — the WebGPU implementation behind Ruby's

GPU device abstraction.

- [FFmpeg](https://ffmpeg.org/) — video and audio decode. Dev builds the link. Homebrew's GPL

  build; it has to be rebuilt LGPL (no `--enable-gpl`, no libx264) before this ships.

- [miniaudio](https://miniaudio.io/) — the audio device backend.

- [nlohmann/json](https://github.com/nlohmann/json) — project files and effect schemas.

- [Lua 5.4](https://www.lua.org/) (MIT) — the scripting and expression sandbox.

- [beat_this_cpp](https://github.com/CPJKU/beat_this) (MIT, including model weights) —

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

## What is not in this repo

This is open core. Three things ship only in the paid build:

- Render and export

- Beat detection and beat editing

- The handcrafted preset packs

The entire effects library is here, deliberately.

A build from this tree will be a real compositor with a complete effect set. You will be able
to cut by hand, build your own presets against exactly the same effects we use, and preview
in real time. You will not be able to detect a beat, cut to one, render a file, or get our
packs.

See [docs/LICENSE-FAQ.md](docs/LICENSE-FAQ.md) for the plain-language version.

## License

[Business Source License 1.1](LICENSE). This is a source available, not open source, and
the distinction matters enough that we are not going to blur it.
Use it for anything, including commercial work, and sell everything you make with it. Do not
take this code and ship a competing editor. Each released version converts to GPL v3
years after its release.

The bundled preset packs are not in this repo and are not covered by this license.

## Contributing

Sign the [CLA](CLA.md) in your first pull request. It is one page, and it exists because
contributed code has to be usable in the paid build. Without it, every contributor keeps
separate copyright over their patch, and the project cannot ship.
Contributions are welcome, but the project is at the stage where most of it is still being
decided. Open an issue before writing anything substantial.

