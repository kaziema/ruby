#pragma once

#include <QImage>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <vector>

#include "ruby/core/Document.h"

namespace ruby::ui {

// Turns a text layer into pixels. Lives in UI because it needs Qt's font machinery;
// the engine only ever sees the resulting QImage.

// One laid-out character: its outline as a path, and where it sits. Per-character
// (not per-line) so each glyph can be transformed independently for animators.
//
// `index`/`bounds` are unused today but cost nothing to keep for future animators.
struct LaidOutGlyph {
    // Empty for glyphs with no outline (colour emoji: bitmaps, not curves). Those draw
    // via drawText at `origin` instead.
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

    // Set when the text was too big to rasterise, distinct from "empty".
    bool tooLarge = false;

    QString requestedFont;
    QString actualFont;

    [[nodiscard]] bool valid() const noexcept { return !image.isNull(); }
};

// Lays text out one character at a time. `pixelsPerPoint` rasterises at the size it
// will actually be seen, avoiding scaled (mushy) type. Caller caches on (text, style,
// scale).
[[nodiscard]] std::vector<LaidOutGlyph> layOutText(const core::Layer& layer,
                                                   double pixelsPerPoint);

[[nodiscard]] TextRaster rasteriseText(const core::Layer& layer, double pixelsPerPoint);

// Rasterised image size without rasterising. Not the same as ink: padded by stroke
// width. Selection/hit-testing/align must use this exact number, or their box won't
// match the drawn quad. Shares the padding logic with rasteriseText.
[[nodiscard]] QSizeF textLayerSize(const core::Layer& layer, double pixelsPerPoint);

}  // namespace ruby::ui
