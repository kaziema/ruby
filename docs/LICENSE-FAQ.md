# License, in plain English

Ruby is under the **Business Source License 1.1**. The legal text is in
[`LICENSE`](LICENSE). This page explains what it actually means. If the two ever
disagree, `LICENSE` wins.

## The short version

Use it for whatever you want. Sell the videos you make with it. Change the code.
Just don't take this code and ship your own video editor with it.

Four years after any version is released, that version becomes GPL v3, which is a
normal open source license.

## Can I use Ruby to make videos and sell them?

Yes. Client work, ads, sponsored content, whatever you want. Everything you create
with it is yours and there is no revenue limit, no royalty, and nothing to report.

## Can I read and change the code?

Yes. Read it, fork it, patch it, compile your own build, use that build for your own
work.

## What can't I do?

You can't take this code and release a competing product with it. No reskinned
version, no hosted version, no "powered by" editor sold under a different name. That
covers video editors, compositors, motion graphics tools, and animation tools built
on or derived from this code.

That's the only thing the license blocks.

## What happens in four years?

Each released version automatically flips to **GPL v3** on its fourth birthday, and
that is permanent. Once a version flips, anyone can build on it, including
commercially. The catch for them is that GPL v3 requires their version to stay open
too, so nobody can take it private.

Versions flip one at a time. Version 1.0 flipping does not flip version 2.0.

## Is this open source?

**No,** and it matters that we say so. "Open source" has a specific meaning, and BSL
does not meet it because of the restriction above. The correct term is
**source available**. The code becomes genuinely open source later, when each version
hits its Change Date and converts to GPL v3.

## Why not just use MIT or Apache?

Because either one would let somebody clone this, put a different name on it, and sell
it, while contributing nothing back. BSL keeps everything else open and blocks exactly
that one thing.

## Why not close the source entirely?

Because there is real value in you being able to read the code, fix your own bugs,
learn from it, and trust what it does with your files. None of that requires letting
someone sell a copy of it.

## Something's missing from this repo

Three things are not here. All three ship only in the paid build.

- **Render and export.** The engine that turns your timeline into a finished file.
- **Beat detection and beat editing.** Audio analysis, the beat grid, snapping to it,
  cutting to it, and the one-click edit builder.
- **The preset packs.** The presets that come with the app. Transitions, shakes, color
  grades, text animations, the lot. Every one made by hand, one at a time.

Everything else is here, and that includes **the entire effects library**. Every effect
the app can run is in this repo. That's on purpose: a preset is worthless if the effect
under it is missing, and "presets always work" is the whole promise. Withholding effects
would break that promise on our own repo.

So a build from this repo is a real compositor with a complete effect set. You can cut an
edit by hand, build your own presets on exactly the same effects we use, and scrub it in
real time. You can't detect a beat, cut to one, render a finished file, or get our packs.

We're not pretending otherwise. The whole package is the product, and those three are the
parts you're paying for.

## Are the bundled presets under this license?

No. They aren't in this repo, so BSL never touches them. They're commercial content that
comes with the paid app, under their own terms.

The preset *format* and the code that loads it are here and are BSL like everything else.
Build your own packs, share them, sell them. That's encouraged.

## I want to contribute

Great. You'll need to sign the [CLA](CLA.md) first, which is one page. It exists
because contributions need to be usable in the paid build, and without it every
contributor would keep separate copyright over their patch and nobody could ship
anything.

## I want to do something the license doesn't allow

Ask. Commercial licenses are available and the answer might just be yes.

---

*This page is a plain-language summary written for clarity, not a legal document, and
it is not legal advice.*
