// panel_layer.h -- the Multiplayer panel's software layer on Linux.
//
// menu_hook.cpp draws the panel into a straight-alpha BGRA layer (rect fills
// with alpha, GDI text used as coverage) and composites it over a readback of
// the game frame. This is the same layer with the same blending; only the text
// changes: stb_truetype over the game's own face (res/fonts/Lato2OFL), with the
// game's Noto faces as fallbacks for glyphs Lato lacks, in place of GDI.
//
// Text is UTF-8 throughout (the lobby speaks UTF-8 JSON), where the Windows
// panel converted to wchar_t for GDI.
#pragma once
#include <cstdint>

namespace layer {

struct Rgb { uint8_t r, g, b; };
constexpr Rgb rgb(int r, int g, int b) { return Rgb{ (uint8_t)r, (uint8_t)g, (uint8_t)b }; }

// The canvas: w x h, BGRA, straight alpha, cleared to transparent.
void Begin(int w, int h);
int Width();
int Height();
const uint8_t* Pixels();

// Fill a rectangle, src-over with alpha a (0..255), clipped to the canvas.
void Rect(int x, int y, int w, int h, Rgb c, int a);

// The DrawTextW flags the panel uses.
enum : unsigned {
    kLeft = 0, kCenter = 1, kRight = 2,   // horizontal
    kTop = 0, kVCenter = 4,               // vertical (single line)
    kWordBreak = 8,                       // wrap at the box width, lines from the top
    kEndEllipsis = 16,                    // single line: cut with an ellipsis to fit
};

// Load the primary face; later calls add fallbacks tried in order for glyphs
// the earlier faces lack. False if the file cannot be read or parsed.
bool AddFont(const char* ttfPath);
bool HaveFont();

// Width of a single line in pixels at `px` (the em height, as CreateFontW(-px)).
int TextWidth(const char* utf8, int px);

// Draw into the box [x, x+w) x [y, y+h), clipped to it like DrawTextW.
void Text(int x, int y, int w, int h, const char* utf8, int px, Rgb c, unsigned flags, int alpha = 255);

}  // namespace layer
