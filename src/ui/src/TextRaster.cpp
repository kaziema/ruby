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

// Ink bounds padded for stroke: the stroke is centred on the outline, so pad by the
// full width (cheap, and leaves room for antialiasing). Stroke clamped to zero — a
// negative value must not shrink the padding.
QRectF paddedInk(const core::Layer& layer, const std::vector<LaidOutGlyph>& glyphs,
                 double pixelsPerPoint) {
    QRectF ink;
    for (const LaidOutGlyph& glyph : glyphs) {
        ink = ink.isNull() ? glyph.bounds : ink.united(glyph.bounds);
    }
    const double stroke = std::max(0.0, layer.strokeWidth) * std::max(0.01, pixelsPerPoint);
    return ink.adjusted(-stroke - 2.0, -stroke - 2.0, stroke + 2.0, stroke + 2.0);
}


// 96 DPI, fixed, so "72pt" means the same thing on every machine/display.
constexpr double kPointsPerInch = 72.0;
constexpr double kDotsPerInch = 96.0;

// Refuse above this rather than allocate gigabytes; 16384 is the common max texture dim.
constexpr int kMaxRasterSide = 16384;

QFont fontFor(const core::Layer& layer, double pixelsPerPoint) {
    QFont font(QString::fromStdString(layer.fontFamily));
    // setPixelSize, not setPointSize, to avoid the platform's DPI. Size/scale clamped
    // positive so a zero/negative input can't produce a garbage smear of pixels.
    const double size = std::max(0.01, layer.fontSize);
    const double scale = std::max(0.01, pixelsPerPoint);
    const double px = size * (kDotsPerInch / kPointsPerInch) * scale;
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(px))));
    return font;
}

// Splits into grapheme clusters, not UTF-16 code units — per-QChar iteration breaks
// surrogate pairs (emoji) and combining marks into separate glyphs.
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

    // Measure every line first: alignment needs the widest one before placing any.
    std::vector<double> widths;
    widths.reserve(static_cast<std::size_t>(lines.size()));
    double widest = 0.0;
    for (const QString& line : lines) {
        double w = fm.horizontalAdvance(line) + tracking * static_cast<double>(std::max<qsizetype>(0, line.size() - 1));

        // Bearing correction: a glyph can overhang its advance width (italics, f/j),
        // which clips the first/last character at the image edge if uncorrected.
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

            // One addText per character: gives per-glyph geometry for animators, no
            // glyph atlas.
            if (!space) {
                LaidOutGlyph glyph;
                glyph.origin = QPointF(x, y);
                glyph.path.addText(glyph.origin, font, ch);

                if (glyph.path.isEmpty()) {
                    // No outline: almost always colour emoji, stored as bitmaps rather
                    // than curves. Bounds come from metrics; geometry drawn later via
                    // drawText.
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
    // Still renders with a substituted font, but flags it so the caller can warn about it.
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
        // Flagged rather than silently returning an invalid raster, so the caller can
        // tell the user the text is too large.
        out.tooLarge = true;
        return out;
    }

    // Premultiplied: the compositor's quad shader expects it, or blending breaks under
    // Add/Screen.
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

    // Stroke first, fill second: the stroke is centred on the outline, so filling after
    // keeps the letterform shape intact.
    if (stroke > 0.0) {
        QPen pen(toColor(layer.strokeColor));
        pen.setWidthF(stroke * 2.0);  // doubled: only the outer half is kept
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (const LaidOutGlyph& glyph : glyphs) {
            // Bitmap glyphs get no stroke: there's no outline, and faking one would draw
            // a rectangle around the emoji.
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

    // Bitmap glyphs drawn last in their own colours; tinting them would make a
    // coloured blob.
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
    // Ceiled: the raster allocates whole pixels, and a box half a pixel short doesn't fit.
    return {std::ceil(padded.width()), std::ceil(padded.height())};
}

}  // namespace ruby::ui
