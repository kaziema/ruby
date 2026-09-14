#pragma once

#include <QImage>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <vector>

#include "ruby/core/Document.h"

namespace ruby::ui {

// Turns a text layer into pixels.
//
// Lives in the UI module because it needs Qt's font machinery, which is the only text
// stack already in the tree and is a genuinely good one. The engine takes the QImage and
// never learns what a glyph is.

// One laid-out character: its outline as a path, and where it sits.
//
// This is the reason layout runs per character rather than per line, which is what MLT's
// qtext filter does. A path per glyph costs one extra loop and produces exactly the
// geometry After Effects' text animators need: each character can be transformed on its
// own before it is filled. That is per-character animation with no glyph atlas and no GPU
// text pipeline, and it is the difference between "captions" and "captions that pop per
// word", which is the thing this app exists for.
//
// Nothing consumes `index` or `bounds` yet. They are here because leaving them out would
// mean rewriting the layout when animators arrive, and they cost nothing now.
struct LaidOutGlyph {
    // Empty for glyphs that have no outline. Colour emoji are the case that matters: they
    // are bitmaps in the font, not curves, so addText produces nothing for them. Those are
    // drawn with drawText at `origin` instead, which still allows a per-glyph transform.
    QPainterPath path;
    QString bitmapText;
    QPointF origin;

    QRectF bounds;
    int index = 0;       // position in the source string
    int wordIndex = 0;   // for animators that step by word
    int lineIndex = 0;   // and by line
};

struct TextRaster {
    QImage image;        // premultiplied RGBA, transparent where there is no ink
    QRectF inkBounds;    // the drawn extent, before padding
    bool fontSubstituted = false;

    // Set when the text was too big to rasterise. Distinguishes "too large" from "empty",
    // which both used to look like an invalid raster and mean very different things.
    bool tooLarge = false;

    QString requestedFont;
    QString actualFont;

    [[nodiscard]] bool valid() const noexcept { return !image.isNull(); }
};

// Lays the layer's text out one character at a time.
//
// `pixelsPerPoint` is how big a point should be on the target, so text is rasterised at
// the size it will actually be seen rather than at a fixed size that then gets scaled.
// Olive does this and the reason is obvious once you have looked at scaled type: it is
// mush. The caller is responsible for caching on (text, style, scale).
[[nodiscard]] std::vector<LaidOutGlyph> layOutText(const core::Layer& layer,
                                                   double pixelsPerPoint);

[[nodiscard]] TextRaster rasteriseText(const core::Layer& layer, double pixelsPerPoint);

// How big the rasterised image will be, without rasterising it.
//
// A text layer's size is NOT its ink: the raster pads by the stroke width plus a couple of
// pixels so a heavy outline is not clipped, and the compositor sizes the layer's quad from
// the image it is handed. Anything that needs to know where a text layer is on screen -
// selection handles, hit testing, align - has to use this same number, or it draws a box
// around a rectangle that is not the one being drawn.
//
// Factored out of rasteriseText rather than reimplemented beside it, so the padding rule
// cannot be changed in one place and not the other.
[[nodiscard]] QSizeF textLayerSize(const core::Layer& layer, double pixelsPerPoint);

}  // namespace ruby::ui
