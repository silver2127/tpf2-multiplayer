// panel_layer.cpp -- see panel_layer.h.
#include "panel_layer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"

namespace layer {

// ---- canvas -------------------------------------------------------------------
static int g_w = 0, g_h = 0;
static std::vector<uint8_t> g_px;

void Begin(int w, int h)
{
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    g_w = w; g_h = h;
    g_px.assign((size_t)w * (size_t)h * 4, 0);
}
int Width() { return g_w; }
int Height() { return g_h; }
const uint8_t* Pixels() { return g_px.data(); }

// src-over onto a straight-alpha BGRA pixel (menu_hook.cpp: pxOver)
static inline void PxOver(uint8_t* d, int r, int g, int b, int a)
{
    if (a <= 0) return;
    int da = d[3];
    if (a >= 255 || da == 0) { d[0] = (uint8_t)b; d[1] = (uint8_t)g; d[2] = (uint8_t)r; d[3] = (uint8_t)a; return; }
    int k = da * (255 - a) / 255, outA = a + k;
    if (outA <= 0) return;
    d[0] = (uint8_t)((b * a + d[0] * k) / outA);
    d[1] = (uint8_t)((g * a + d[1] * k) / outA);
    d[2] = (uint8_t)((r * a + d[2] * k) / outA);
    d[3] = (uint8_t)outA;
}

void PlaceOnBackdrop(int w,int h,int x,int y,const uint8_t* bg)
{
    std::vector<uint8_t> out(bg,bg+size_t(w)*h*4);
    for(int py=0;py<g_h;++py)for(int px=0;px<g_w;++px) {
        if(x+px<0 || y+py<0 || x+px>=w || y+py>=h)continue;
        const auto* s=g_px.data()+(size_t(py)*g_w+px)*4;
        auto* d=out.data()+(size_t(y+py)*w+x+px)*4;
        PxOver(d,s[2],s[1],s[0],s[3]);
    }
    g_px.swap(out);g_w=w;g_h=h;
}

void Rect(int x, int y, int w, int h, Rgb c, int a)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_w ? g_w : x + w, y1 = y + h > g_h ? g_h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint8_t* d = g_px.data() + ((size_t)yy * g_w + x0) * 4;
        for (int xx = x0; xx < x1; xx++, d += 4) PxOver(d, c.r, c.g, c.b, a);
    }
}

// ---- fonts --------------------------------------------------------------------
struct Face {
    std::vector<unsigned char> data;
    stbtt_fontinfo info;
};
static std::vector<Face*> g_faces;   // never freed: the panel lives as long as the game

bool AddFont(const char* ttfPath)
{
    FILE* f = fopen(ttfPath, "rb");
    if (!f) return false;
    auto* face = new Face;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        face->data.resize((size_t)n);
        if (fread(face->data.data(), 1, (size_t)n, f) != (size_t)n) face->data.clear();
    }
    fclose(f);
    if (face->data.empty() ||
        !stbtt_InitFont(&face->info, face->data.data(), stbtt_GetFontOffsetForIndex(face->data.data(), 0))) {
        delete face;
        return false;
    }
    g_faces.push_back(face);
    return true;
}

bool HaveFont() { return !g_faces.empty(); }

// One rendered glyph at one pixel size.
struct Glyph {
    int face = -1;            // index into g_faces, -1 = no face has it
    int glyph = 0;            // glyph index in that face
    float advance = 0;        // pixels
    int x0 = 0, y0 = 0;       // bitmap offset from the pen, baseline-relative
    int w = 0, h = 0;
    std::vector<uint8_t> coverage;
};
static std::unordered_map<uint64_t, Glyph> g_glyphs;

static float ScaleFor(const Face* face, int px)
{
    return stbtt_ScaleForMappingEmToPixels(&face->info, (float)px);
}

static const Glyph& GlyphFor(uint32_t cp, int px)
{
    const uint64_t key = ((uint64_t)(uint32_t)px << 32) | cp;
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return it->second;
    Glyph g;
    for (size_t i = 0; i < g_faces.size(); i++) {
        int gi = stbtt_FindGlyphIndex(&g_faces[i]->info, (int)cp);
        if (gi == 0) continue;
        g.face = (int)i;
        g.glyph = gi;
        break;
    }
    if (g.face < 0 && !g_faces.empty() && cp != '?') {   // nothing has it: draw what DrawTextW would, a placeholder
        Glyph q = GlyphFor('?', px);
        return g_glyphs.emplace(key, q).first->second;
    }
    if (g.face >= 0) {
        const stbtt_fontinfo& fi = g_faces[(size_t)g.face]->info;
        const float s = ScaleFor(g_faces[(size_t)g.face], px);
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&fi, g.glyph, &adv, &lsb);
        g.advance = adv * s;
        int x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&fi, g.glyph, s, s, &g.x0, &g.y0, &x1, &y1);
        g.w = x1 - g.x0;
        g.h = y1 - g.y0;
        if (g.w > 0 && g.h > 0) {
            g.coverage.resize((size_t)g.w * (size_t)g.h);
            stbtt_MakeGlyphBitmap(&fi, g.coverage.data(), g.w, g.h, g.w, s, s, g.glyph);
        }
    }
    return g_glyphs.emplace(key, std::move(g)).first->second;
}

// Ascent and descent (descent negative) of the primary face at px.
static void LineMetrics(int px, float* ascent, float* descent, float* gap)
{
    *ascent = (float)px * 0.8f; *descent = -(float)px * 0.2f; *gap = 0;
    if (g_faces.empty()) return;
    int a = 0, d = 0, l = 0;
    stbtt_GetFontVMetrics(&g_faces[0]->info, &a, &d, &l);
    const float s = ScaleFor(g_faces[0], px);
    *ascent = a * s; *descent = d * s; *gap = l * s;
}

static std::vector<uint32_t> Decode(const char* s)
{
    std::vector<uint32_t> out;
    const unsigned char* p = (const unsigned char*)(s ? s : "");
    while (*p) {
        uint32_t cp = *p;
        int more = 0;
        if (cp >= 0xF0) { cp &= 0x07; more = 3; }
        else if (cp >= 0xE0) { cp &= 0x0F; more = 2; }
        else if (cp >= 0xC0) { cp &= 0x1F; more = 1; }
        else if (cp >= 0x80) { out.push_back('?'); p++; continue; }   // stray continuation byte
        p++;
        bool ok = true;
        for (int i = 0; i < more; i++) {
            if ((p[0] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (p[0] & 0x3F);
            p++;
        }
        out.push_back(ok ? cp : (uint32_t)'?');
    }
    return out;
}

static float Kern(uint32_t a, uint32_t b, int px)
{
    const Glyph& ga = GlyphFor(a, px);
    const Glyph& gb = GlyphFor(b, px);
    if (ga.face < 0 || ga.face != gb.face) return 0;
    const Face* face = g_faces[(size_t)ga.face];
    return stbtt_GetGlyphKernAdvance(&face->info, ga.glyph, gb.glyph) * ScaleFor(face, px);
}

static float RunWidth(const uint32_t* cps, size_t n, int px)
{
    float w = 0;
    for (size_t i = 0; i < n; i++) {
        w += GlyphFor(cps[i], px).advance;
        if (i + 1 < n) w += Kern(cps[i], cps[i + 1], px);
    }
    return w;
}

int TextWidth(const char* utf8, int px)
{
    const std::vector<uint32_t> cps = Decode(utf8);
    return (int)std::ceil(RunWidth(cps.data(), cps.size(), px));
}

// Blend one line with its pen starting at (penX, baseline), clipped to the box.
static void DrawRun(const uint32_t* cps, size_t n, float penX, int baseline, int px, Rgb c, int alpha,
                    int bx0, int by0, int bx1, int by1)
{
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > g_w) bx1 = g_w;
    if (by1 > g_h) by1 = g_h;
    for (size_t i = 0; i < n; i++) {
        const Glyph& g = GlyphFor(cps[i], px);
        const int gx = (int)std::lround(penX) + g.x0;
        const int gy = baseline + g.y0;
        for (int yy = 0; yy < g.h; yy++) {
            const int ly = gy + yy;
            if (ly < by0 || ly >= by1) continue;
            for (int xx = 0; xx < g.w; xx++) {
                const int lx = gx + xx;
                if (lx < bx0 || lx >= bx1) continue;
                const int m = g.coverage[(size_t)yy * g.w + xx];
                if (m) PxOver(g_px.data() + ((size_t)ly * g_w + lx) * 4, c.r, c.g, c.b, m * alpha / 255);
            }
        }
        penX += g.advance;
        if (i + 1 < n) penX += Kern(cps[i], cps[i + 1], px);
    }
}

void Text(int x, int y, int w, int h, const char* utf8, int px, Rgb c, unsigned flags, int alpha)
{
    if (w <= 0 || h <= 0 || px <= 0 || g_faces.empty()) return;
    std::vector<uint32_t> cps = Decode(utf8);
    float ascent, descent, gap;
    LineMetrics(px, &ascent, &descent, &gap);
    const int bx1 = x + w, by1 = y + h;

    if (flags & kWordBreak) {
        // Greedy wrap at spaces (a word longer than the box is cut per glyph),
        // lines from the top, as DT_WORDBREAK | DT_TOP.
        const float lineH = ascent - descent + gap;
        float lineTop = (float)y;
        size_t i = 0;
        while (i < cps.size() && lineTop < by1) {
            while (i < cps.size() && cps[i] == ' ') i++;
            size_t end = i, lastSpace = 0;
            bool haveSpace = false;
            while (end < cps.size() && cps[end] != '\n') {
                if (RunWidth(cps.data() + i, end - i + 1, px) > (float)w && end > i) break;
                if (cps[end] == ' ') { lastSpace = end; haveSpace = true; }
                end++;
            }
            size_t cut = end;
            if (end < cps.size() && cps[end] != '\n' && haveSpace) cut = lastSpace;
            float lw = RunWidth(cps.data() + i, cut - i, px);
            float penX = (float)x;
            if (flags & kCenter) penX = x + (w - lw) / 2;
            else if (flags & kRight) penX = x + w - lw;
            DrawRun(cps.data() + i, cut - i, penX, (int)std::lround(lineTop + ascent), px, c, alpha, x, y, bx1, by1);
            lineTop += lineH;
            i = (cut < cps.size() && (cps[cut] == ' ' || cps[cut] == '\n')) ? cut + 1 : cut;
        }
        return;
    }

    float tw = RunWidth(cps.data(), cps.size(), px);
    if ((flags & kEndEllipsis) && tw > (float)w) {
        static const uint32_t kEllipsis = 0x2026;
        const float ew = GlyphFor(kEllipsis, px).advance;
        while (!cps.empty() && RunWidth(cps.data(), cps.size(), px) + ew > (float)w) cps.pop_back();
        cps.push_back(kEllipsis);
        tw = RunWidth(cps.data(), cps.size(), px);
    }
    float penX = (float)x;
    if (flags & kCenter) penX = x + (w - tw) / 2;
    else if (flags & kRight) penX = x + w - tw;
    const float textH = ascent - descent;
    const float top = (flags & kVCenter) ? y + (h - textH) / 2 : (float)y;
    DrawRun(cps.data(), cps.size(), penX, (int)std::lround(top + ascent), px, c, alpha, x, y, bx1, by1);
}

}  // namespace layer
