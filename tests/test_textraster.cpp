// Text layout and rasterisation. Needs fonts + QGuiApplication; reports 77/SKIPPED
// without a platform plugin so headless CI doesn't fail on an environment issue.

#include <QGuiApplication>
#include <QImage>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "ruby/ui/TextRaster.h"

using namespace ruby;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

core::Layer textLayer(const char* text) {
    core::Layer layer;
    layer.kind = core::LayerKind::Text;
    layer.text = text;
    layer.fontSize = 48.0;
    layer.textColor = core::Value::rgba(1.0, 1.0, 1.0, 1.0);
    return layer;
}

// Layout must produce one addressable path per glyph.
void layout_produces_one_path_per_visible_character() {
    const std::vector<ui::LaidOutGlyph> glyphs = ui::layOutText(textLayer("AB C"), 1.0);
    check(glyphs.size() == 3, "three visible glyphs; the space produces none");

    for (const ui::LaidOutGlyph& g : glyphs) {
        check(!g.path.isEmpty(), "each glyph has real geometry");
        check(g.bounds.width() > 0.0 && g.bounds.height() > 0.0,
              "and a non-empty bounding box");
    }
    check(glyphs[0].bounds.left() < glyphs[1].bounds.left(),
          "glyphs advance left to right");
}

// Glyph indices must match the source string's character indices.
void indices_line_up_with_the_source_string() {
    const std::vector<ui::LaidOutGlyph> glyphs = ui::layOutText(textLayer("ab cd"), 1.0);
    check(glyphs.size() == 4, "four visible glyphs");
    check(glyphs[0].index == 0 && glyphs[1].index == 1, "first word indexed from zero");
    check(glyphs[2].index == 3 && glyphs[3].index == 4,
          "the space still occupies an index, so indices match the string");

    check(glyphs[0].wordIndex == 0 && glyphs[1].wordIndex == 0, "first word is word 0");
    check(glyphs[2].wordIndex == 1 && glyphs[3].wordIndex == 1, "second word is word 1");
}

void lines_are_counted_and_stacked() {
    const std::vector<ui::LaidOutGlyph> glyphs = ui::layOutText(textLayer("a\nb"), 1.0);
    check(glyphs.size() == 2, "one glyph per line");
    check(glyphs[0].lineIndex == 0 && glyphs[1].lineIndex == 1, "line indices increment");
    check(glyphs[1].bounds.top() > glyphs[0].bounds.top(),
          "and the second line sits below the first");
}

void alignment_moves_the_shorter_line() {
    core::Layer layer = textLayer("mmmm\ni");

    layer.textAlign = core::TextAlign::Left;
    const auto left = ui::layOutText(layer, 1.0);
    layer.textAlign = core::TextAlign::Right;
    const auto right = ui::layOutText(layer, 1.0);
    layer.textAlign = core::TextAlign::Center;
    const auto centre = ui::layOutText(layer, 1.0);

    check(!left.empty() && !right.empty() && !centre.empty(), "all three laid out");

    // The narrow second line is the one that moves; the wide first line defines the box.
    const double leftI = left.back().bounds.left();
    const double centreI = centre.back().bounds.left();
    const double rightI = right.back().bounds.left();
    check(leftI < centreI && centreI < rightI,
          "left, centre and right place the short line progressively further right");
}

void tracking_spreads_characters() {
    core::Layer layer = textLayer("iii");
    const auto tight = ui::layOutText(layer, 1.0);
    layer.tracking = 40.0;
    const auto loose = ui::layOutText(layer, 1.0);

    check(tight.size() == 3 && loose.size() == 3, "same glyph count");
    check(loose.back().bounds.left() > tight.back().bounds.left(),
          "positive tracking pushes later characters further out");
}

void rasterising_produces_premultiplied_pixels_with_ink() {
    const ui::TextRaster raster = ui::rasteriseText(textLayer("Hello"), 1.0);
    check(raster.valid(), "a raster came out");
    check(raster.image.format() == QImage::Format_RGBA8888_Premultiplied,
          "premultiplied, which is what the compositor's quad shader expects");

    // Some pixel has to be opaque, or we drew nothing and would never know.
    bool anyInk = false;
    for (int y = 0; y < raster.image.height() && !anyInk; ++y) {
        for (int x = 0; x < raster.image.width(); ++x) {
            if (qAlpha(raster.image.pixel(x, y)) > 128) {
                anyInk = true;
                break;
            }
        }
    }
    check(anyInk, "and there is actually ink on it");

    const ui::TextRaster empty = ui::rasteriseText(textLayer(""), 1.0);
    check(!empty.valid(), "empty text rasterises to nothing rather than a blank image");
}

// A missing font must be reported, not silently substituted.
void a_missing_font_is_reported() {
    core::Layer layer = textLayer("Hi");
    layer.fontFamily = "ThisFontDoesNotExistAnywhere12345";
    const ui::TextRaster raster = ui::rasteriseText(layer, 1.0);

    check(raster.valid(), "it still renders, in whatever the system substituted");
    check(raster.fontSubstituted, "and says the font was substituted");
    check(raster.requestedFont == QStringLiteral("ThisFontDoesNotExistAnywhere12345"),
          "reporting what was asked for");
    check(!raster.actualFont.isEmpty(), "and what was used instead");
}

void scale_changes_the_rendered_size() {
    const ui::TextRaster small = ui::rasteriseText(textLayer("Hello"), 1.0);
    const ui::TextRaster large = ui::rasteriseText(textLayer("Hello"), 2.0);
    check(small.valid() && large.valid(), "both rasterised");
    check(large.image.width() > small.image.width(),
          "rasterising at a larger scale produces more pixels, rather than the same "
          "pixels scaled up");
}


// Layout steps by grapheme cluster, not UTF-16 code unit (regression: QChar iteration split emoji/accents).
void one_glyph_per_user_perceived_character() {
    check(ui::layOutText(textLayer("\xF0\x9F\x8E\xAC"), 1.0).size() == 1,
          "an emoji is one glyph, not two surrogate halves");
    check(ui::layOutText(textLayer("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"), 1.0).size() == 1,
          "a flag is one glyph, not two regional indicators");
    check(ui::layOutText(textLayer(
              "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA6"), 1.0)
              .size() == 1,
          "a zero-width-joiner sequence is one glyph");
    check(ui::layOutText(textLayer("e\xCC\x81 a\xCC\x81"), 1.0).size() == 2,
          "a letter and its combining accent are one glyph, not two");
}

// Colour emoji are font bitmaps with no outline path; regression: used to rasterise empty.
void emoji_actually_put_pixels_down() {
    const auto inked = [](const ui::TextRaster& r) {
        int count = 0;
        for (int y = 0; y < r.image.height(); ++y) {
            for (int x = 0; x < r.image.width(); ++x) {
                if (qAlpha(r.image.pixel(x, y)) > 64) ++count;
            }
        }
        return count;
    };

    const ui::TextRaster emoji = ui::rasteriseText(textLayer("\xF0\x9F\x8E\xAC"), 1.0);
    check(emoji.valid(), "an emoji-only layer rasterises");
    check(emoji.image.width() > 16 && emoji.image.height() > 16,
          "at a real size rather than a few pixels");
    check(inked(emoji) > 100, "and actually draws something");

    const ui::TextRaster mixed = ui::rasteriseText(textLayer("GO \xF0\x9F\x8E\xAC"), 1.0);
    check(inked(mixed) > inked(emoji), "text plus emoji draws more than emoji alone");
}

// Too-large and empty both used to produce an invalid raster indistinguishably.
void oversized_text_says_so_instead_of_vanishing() {
    core::Layer huge = textLayer("x");
    huge.text = std::string(2000, 'W');
    const ui::TextRaster raster = ui::rasteriseText(huge, 1.0);
    check(!raster.valid(), "it does not produce an image");
    check(raster.tooLarge, "but it reports why, rather than looking like empty text");

    const ui::TextRaster empty = ui::rasteriseText(textLayer(""), 1.0);
    check(!empty.valid() && !empty.tooLarge, "empty text is empty, not too large");
}

// Degenerate inputs used to produce a smear of pixels instead of a clean rejection.
void degenerate_inputs_are_clamped() {
    for (const double scale : {0.0, -1.0}) {
        const ui::TextRaster r = ui::rasteriseText(textLayer("x"), scale);
        check(r.valid(), "a non-positive scale still produces something rather than crashing");
    }
    for (const double size : {0.0, -10.0}) {
        core::Layer layer = textLayer("x");
        layer.fontSize = size;
        check(ui::rasteriseText(layer, 1.0).valid(), "so does a non-positive font size");
    }

    // Regression: negative stroke fed negative padding, shrinking and clipping the image.
    core::Layer negative = textLayer("stroke");
    negative.strokeWidth = -5.0;
    core::Layer none = textLayer("stroke");
    none.strokeWidth = 0.0;
    const ui::TextRaster a = ui::rasteriseText(negative, 1.0);
    const ui::TextRaster b = ui::rasteriseText(none, 1.0);
    check(a.valid() && b.valid(), "both rasterise");
    check(a.image.size() == b.image.size(),
          "a negative stroke is treated as no stroke, not as negative padding");
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    layout_produces_one_path_per_visible_character();
    indices_line_up_with_the_source_string();
    lines_are_counted_and_stacked();
    alignment_moves_the_shorter_line();
    tracking_spreads_characters();
    rasterising_produces_premultiplied_pixels_with_ink();
    a_missing_font_is_reported();
    scale_changes_the_rendered_size();
    one_glyph_per_user_perceived_character();
    emoji_actually_put_pixels_down();
    oversized_text_says_so_instead_of_vanishing();
    degenerate_inputs_are_clamped();

    if (failures != 0) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("textraster: all checks passed");
    return EXIT_SUCCESS;
}
