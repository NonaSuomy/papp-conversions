// Text for the Scratch Everywhere! PAPP renderer: UTF-8 strings drawn with
// the 8x8 font8x8 glyphs (papp_font.c), scaled nearest-neighbour.
// Characters outside Latin-1 are drawn as '?'.
#include "papp_raster.h"
#include "papp_render.h"

#include <text_papp.hpp>

#include <math.h>
#include <string.h>

// papp_font.c: the glyph for a code point ('?' outside Latin-1), 8 rows of
// 8 bits, least significant bit leftmost.
extern "C" const char *papp_font_glyph(uint32_t cp);

static const char *glyph(uint32_t cp)
{
    return papp_font_glyph(cp);
}

// Next code point of a UTF-8 string (invalid bytes come out as themselves).
static uint32_t next_cp(const unsigned char *&p)
{
    const uint32_t c = *p++;
    if (c < 0x80) {
        return c;
    }
    int extra = c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : (c >= 0xC0 ? 1 : 0));
    uint32_t cp = extra == 3 ? (c & 0x07) : (extra == 2 ? (c & 0x0F) : (c & 0x1F));
    if (extra == 0) {
        return c;
    }
    while (extra-- > 0 && (*p & 0xC0) == 0x80) {
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    return cp;
}

namespace raster {

void textSize(const char *utf8, float px, int *w, int *h)
{
    int cols = 0, maxCols = 0, lines = 1;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(utf8);
    while (*p) {
        const uint32_t cp = next_cp(p);
        if (cp == '\n') {
            lines++;
            cols = 0;
            continue;
        }
        cols++;
        if (cols > maxCols) {
            maxCols = cols;
        }
    }
    if (w) {
        *w = (int)(maxCols * px + 0.5f);
    }
    if (h) {
        *h = lines * lineHeight(px);
    }
}

void text(const Target &t, const char *utf8, int x, int y, float px, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a == 0 || !(px > 0)) {
        return;
    }
    const float inv = 8.0f / px;  // glyph bits per target pixel
    const int cell = (int)ceilf(px);
    const uint16_t col = pack565(r, g, b);
    const int lh = lineHeight(px);
    const int top = y + (lh - (int)(px + 0.5f)) / 2;  // centre the cell in the line
    int col_i = 0, line = 0;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(utf8);
    while (*p) {
        const uint32_t cp = next_cp(p);
        if (cp == '\n') {
            line++;
            col_i = 0;
            continue;
        }
        const char *gl = glyph(cp);
        const int gx = x + (int)(col_i * px);
        const int gy = top + line * lh;
        col_i++;
        if (gx >= t.w || gy >= t.h || gx + cell <= 0 || gy + cell <= 0) {
            continue;
        }
        for (int yy = 0; yy < cell; yy++) {
            const int ty = gy + yy;
            const int bitRow = (int)(yy * inv);
            if (ty < 0 || ty >= t.h || bitRow > 7) {
                continue;
            }
            const unsigned char bits = (unsigned char)gl[bitRow];
            if (bits == 0) {
                continue;
            }
            for (int xx = 0; xx < cell; xx++) {
                const int tx = gx + xx;
                const int bit = (int)(xx * inv);
                if (tx < 0 || tx >= t.w || bit > 7 || !((bits >> bit) & 1)) {
                    continue;
                }
                if (t.rgb565 != nullptr) {
                    uint16_t &d = t.rgb565[ty * t.w + tx];
                    d = a >= 250 ? col : blend565(d, col, (a + 4) >> 3);
                } else {
                    uint32_t &d = t.rgba[ty * t.w + tx];
                    d = overRGBA(d, r, g, b, a);
                }
            }
        }
    }
}

}  // namespace raster

// ── The runtime's text objects (include/text.hpp) ───────────────────────────

TextObjectPapp::TextObjectPapp(std::string txt, double posX, double posY, std::string fontPath)
    : TextObject(txt, posX, posY, fontPath)
{
}

void TextObjectPapp::setText(std::string txt)
{
    text = txt;
}

void TextObjectPapp::render(int xPos, int yPos)
{
    if (text.empty()) {
        return;
    }
    const float px = BASE_PX * scale;
    int w = 0, h = 0;
    raster::textSize(text.c_str(), px, &w, &h);
    if (centerAligned) {
        xPos -= w / 2;
        yPos -= h / 2;
    }
    // Math::color: 0xRRGGBBAA.
    const uint32_t c = static_cast<uint32_t>(color);
    raster::text(papp_render_target(), text.c_str(), xPos, yPos, px, (c >> 24) & 0xFF, (c >> 16) & 0xFF,
                 (c >> 8) & 0xFF, c & 0xFF);
}

std::vector<float> TextObjectPapp::getSize()
{
    return getStringSize(text);
}

std::vector<float> TextObjectPapp::getStringSize(const std::string &txt)
{
    if (txt.empty()) {
        return {0.0f, 0.0f};
    }
    int w = 0, h = 0;
    raster::textSize(txt.c_str(), BASE_PX * scale, &w, &h);
    return {static_cast<float>(w), static_cast<float>(h)};
}
