#pragma once

#include <QColor>
#include <QRect>

class QPainter;

namespace ruby::ui {

// Drawn as vector paths, not font glyphs: some glyphs have emoji presentation and
// rendered in full color, breaking the monochrome toolbar.

enum class ToolIcon {
    // Reserved for the project selector; drawn but inert, dimmed to read as "not yet".
    Home,

    Selection,
    Hand,
    Zoom,
    Rotation,
    Anchor,
    Text,
    Shape,
    Pen,

    // Panel switches, not tools: change the left dock rather than click behavior.
    // Same bar, divided from the tools to keep the difference visible.
    Project,
    Effects,
};

// Paints the icon centered in `box`, tinted `color`; geometry is authored in a
// 16x16 grid and scaled to fit.
void paintToolIcon(QPainter& p, const QRect& box, ToolIcon icon, const QColor& color);

}  // namespace ruby::ui
