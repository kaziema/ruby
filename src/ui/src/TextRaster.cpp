#include "ruby/ui/TextRaster.h"

#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <cmath>

namespace ruby::ui {
namespace {

// The ink of a laid-out string, padded the way the raster pads it.
//
// The stroke is centred on the outline, so half of it sits outside the glyph. Padding by
// the full width rather than half is cheap and leaves room for antialiasing too.
//
// Clamped at zero: a negative stroke draws nothing, correctly, but used to feed a negative
// number in here, which SHRANK the image and clipped the glyphs it was meant to make room
// for.
QRectF paddedInk(const core::Layer& layer, const std::vector<LaidOutGlyph>& glyphs,
                 double pixelsPerPoint) {
    QRectF ink;
    for (const LaidOutGlyph& glyph : glyphs) {
        ink = ink.isNull() ? glyph.bounds : ink.united(glyph.bounds);
    }
    const double stroke = std::max(0.0, layer.strokeWidth) * std::max(0.01, pixelsPerPoint);
    return ink.adjusted(-stroke - 2.0, -stroke - 2.0, stroke + 2.0, stroke + 2.0);
}


// 96 DPI, fixed. A "72pt" heading has to mean the same thing on every machine, and
// leaving it to the platform means the same project renders differently on two displays.
// Olive pins this for the same reason.
constexpr double kPointsPerInch = 72.0;
constexpr double kDotsPerInch = 96.0;

// Above this we refuse rather than allocating gigabytes. 16384 is the common maximum
// texture dimension, so anything larger could not be uploaded anyway.
constexpr int kMaxRasterSide = 16384;

QFont fontFor(const core::Layer& layer, double pixelsPerPoint) {
    QFont font(QString::fromStdString(layer.fontFamily));
    // setPixelSize rather than setPointSize: point size goes through the platform's DPI,
    // which is exactly the variable we are trying to remove.
    //
    // Both inputs are clamped positive. A zero or negative scale or font size used to
    // produce a tiny smear of pixels instead of either nothing or an error, which is the
    // worst of the three: it looks like the text rendered and went wrong.
    const double size = std::max(0.01, layer.fontSize);
    const double scale = std::max(0.01, pixelsPerPoint);
    const double px = size * (kDotsPerInch / kPointsPerInch) * scale;
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(px))));
    return font;
}

// Splits a line into user-perceived characters, not UTF-16 code units.
//
// Iterating QChar looked right and was wrong twice over. An emoji is a surrogate pair, so
// per-QChar layout draws two halves of a broken glyph; `e` plus a combining acute is two
// code points that are one letter, and splitting them puts the accent in its own box. Both
// show up immediately in the kind of captions this app is for.
//
// A grapheme cluster is exactly "what a person would call one character", which is also
// exactly what an animator should step by.
std::vector<QString> graphemes(const QString& line) {
    std::vector<QString> out;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, line);
    int start = 0;
    while (true) {
        const int next = finder.toNextBoundary();
        if (next < 0) {
            break;
        }
        if (next > start) {
            out.push_back(line.mid(start, next - start));
            start = next;
        }
    }
    if (start < line.size()) {
        out.push_back(line.mid(start));
    }
    return out;
}

}  // namespace

std::vector<LaidOutGlyph> layOutText(const core::Layer& layer, double pixelsPerPoint) {
    std::vector<LaidOutGlyph> glyphs;
    const QString text = QString::fromStdString(layer.text);
    if (text.isEmpty()) {
        return glyphs;
    }

    const QFont font = fontFor(layer, pixelsPerPoint);
    const QFontMetricsF fm(font);
    const double tracking =
        layer.tracking * (kDotsPerInch / kPointsPerInch) * std::max(0.01, pixelsPerPoint);
    const double lineStep = fm.lineSpacing() * layer.lineHeight;

    const QStringList lines = text.split(QLatin1Char('\n'));

    // Measure every line first, because alignment needs to know the widest one before it
    // can place any of them.
    std::vector<double> widths;
    widths.reserve(static_cast<std::size_t>(lines.size()));
    double widest = 0.0;
    for (const QString& line : lines) {
        double w = fm.horizontalAdvance(line) + tracking * static_cast<double>(std::max<qsizetype>(0, line.size() - 1));

        // Bearing correction, taken from MLT's qtext filter. A glyph can overhang its own
        // advance width: italics lean past it, and letters like f and j reach left. Left
        // alone, the first and last characters of a line get clipped at the edge of the
        // image, and only for some fonts, which makes it look like a font bug.
        if (!line.isEmpty()) {
            const double lead = fm.leftBearing(line.front());
            const double trail = fm.rightBearing(line.back());
            if (lead < 0.0) w -= lead;
            if (trail < 0.0) w -= trail;
        }
        widths.push_back(w);
        widest = std::max(widest, w);
    }

    int charIndex = 0;
    int wordIndex = 0;
    double y = fm.ascent();

    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const QString& line = lines[static_cast<int>(lineIndex)];
        const double lineWidth = widths[static_cast<std::size_t>(lineIndex)];

        double x = 0.0;
        switch (layer.textAlign) {
            case core::TextAlign::Left:   x = 0.0;                      break;
            case core::TextAlign::Center: x = (widest - lineWidth) / 2; break;
            case core::TextAlign::Right:  x = widest - lineWidth;       break;
        }
        if (!line.isEmpty()) {
            const double lead = fm.leftBearing(line.front());
            if (lead < 0.0) {
                x -= lead;
            }
        }

        bool inWord = false;
        for (const QString& ch : graphemes(line)) {
            const bool space = ch.at(0).isSpace();
            if (!space && !inWord) {
                inWord = true;
            } else if (space && inWord) {
                inWord = false;
                ++wordIndex;
            }

            // One addText per character. This is the whole point: a path per glyph is the
            // geometry an animator needs, and it costs one loop instead of a glyph atlas.
            if (!space) {
                LaidOutGlyph glyph;
                glyph.origin = QPointF(x, y);
                glyph.path.addText(glyph.origin, font, ch);

                if (glyph.path.isEmpty()) {
                    // No outline. Almost always a colour emoji, which fonts store as a
                    // bitmap rather than curves, so addText has nothing to give. Left
                    // alone these vanished, and captions in this app are full of them.
                    // Metrics still know how big it is, so the box is right even though
                    // the geometry has to come from drawText later.
                    glyph.bitmapText = ch;
                    glyph.bounds = QRectF(x, y - fm.ascent(), fm.horizontalAdvance(ch),
                                          fm.ascent() + fm.descent());
                } else {
                    glyph.bounds = glyph.path.boundingRect();
                }
                glyph.index = charIndex;
                glyph.wordIndex = wordIndex;
                glyph.lineIndex = lineIndex;
                glyphs.push_back(std::move(glyph));
            }
            x += fm.horizontalAdvance(ch) + tracking;
            charIndex += static_cast<int>(ch.size());
        }
        if (inWord) {
            ++wordIndex;
        }
        ++charIndex;  // the newline itself, so indices match the source string
        y += lineStep;
    }
    return glyphs;
}

TextRaster rasteriseText(const core::Layer& layer, double pixelsPerPoint) {
    TextRaster out;
    out.requestedFont = QString::fromStdString(layer.fontFamily);

    const QFont layerFont = fontFor(layer, pixelsPerPoint);
    // A project that opens in the wrong typeface with no warning is worse than one that
    // refuses to open. We still render, but the caller can say which font is missing.
    out.actualFont = QFontInfo(layerFont).family();
    out.fontSubstituted =
        out.actualFont.compare(out.requestedFont, Qt::CaseInsensitive) != 0;

    const std::vector<LaidOutGlyph> glyphs = layOutText(layer, pixelsPerPoint);
    if (glyphs.empty()) {
        return out;
    }

    const QRectF padded = paddedInk(layer, glyphs, pixelsPerPoint);
    const double stroke =
        std::max(0.0, layer.strokeWidth) * std::max(0.01, pixelsPerPoint);

    const int w = std::max(1, static_cast<int>(std::ceil(padded.width())));
    const int h = std::max(1, static_cast<int>(std::ceil(padded.height())));
    if (w > kMaxRasterSide || h > kMaxRasterSide) {
        // Long or enormous text used to return an invalid raster here and simply not
        // appear, with nothing anywhere saying why. Silence is the wrong answer: the
        // caller can now tell the user the text is too large instead of leaving them
        // wondering why their caption vanished.
        out.tooLarge = true;
        return out;
    }

    // Premultiplied, which is what the compositor's quad shader now expects. Anything else
    // and every semi-transparent edge blends wrong under Add and Screen.
    out.image = QImage(w, h, QImage::Format_RGBA8888_Premultiplied);
    out.image.fill(Qt::transparent);
    out.image.setDotsPerMeterX(static_cast<int>(kDotsPerInch / 0.0254));
    out.image.setDotsPerMeterY(static_cast<int>(kDotsPerInch / 0.0254));

    QPainter p(&out.image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(-padded.left(), -padded.top());

    const auto toColor = [](const core::Value& v) {
        return QColor::fromRgbF(std::clamp(v.c[0], 0.0, 1.0), std::clamp(v.c[1], 0.0, 1.0),
                                std::clamp(v.c[2], 0.0, 1.0), std::clamp(v.c[3], 0.0, 1.0));
    };

    // Stroke first, fill second. MLT's comment on this is the clearest statement of why:
    // a stroke is centred on the outline, so drawing it after the fill eats into the glyph
    // from the inside. Painting the fill over it afterwards is what keeps letterforms the
    // shape the designer drew.
    if (stroke > 0.0) {
        QPen pen(toColor(layer.strokeColor));
        pen.setWidthF(stroke * 2.0);  // doubled: only the outer half is kept
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (const LaidOutGlyph& glyph : glyphs) {
            // Bitmap glyphs get no stroke. There is no outline to stroke, and faking one
            // around the emoji's bounding box would draw a rectangle.
            if (!glyph.path.isEmpty()) {
                p.drawPath(glyph.path);
            }
        }
    }

    p.setPen(Qt::NoPen);
    p.setBrush(toColor(layer.textColor));
    for (const LaidOutGlyph& glyph : glyphs) {
        if (!glyph.path.isEmpty()) {
            p.drawPath(glyph.path);
        }
    }

    // Bitmap glyphs last, in their own colours. The fill brush does not apply: an emoji
    // tinted to the text colour would be a coloured blob.
    const QFont font = fontFor(layer, pixelsPerPoint);
    p.setFont(font);
    p.setBrush(Qt::NoBrush);
    p.setPen(toColor(layer.textColor));
    for (const LaidOutGlyph& glyph : glyphs) {
        if (!glyph.bitmapText.isEmpty()) {
            p.drawText(glyph.origin, glyph.bitmapText);
        }
    }
    p.end();

    out.inkBounds = padded;
    return out;
}

QSizeF textLayerSize(const core::Layer& layer, double pixelsPerPoint) {
    const std::vector<LaidOutGlyph> glyphs = layOutText(layer, pixelsPerPoint);
    if (glyphs.empty()) {
        return {};
    }
    const QRectF padded = paddedInk(layer, glyphs, pixelsPerPoint);
    // Ceiled, because the raster allocates whole pixels and the quad is sized from the
    // image. A box half a pixel short of the picture is still a box that does not fit it.
    return {std::ceil(padded.width()), std::ceil(padded.height())};
}

}  // namespace ruby::ui
