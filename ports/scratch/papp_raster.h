// Software rasteriser for the Scratch Everywhere! PAPP renderer.
//
// Everything the renderer draws goes through here: the stage frame is RGB565
// (what the loader's PPA scales to the canvas), the pen layer is RGBA8888
// with straight alpha (the same byte order as the runtime's images: R, G, B,
// A in memory). No allocations: every call draws straight into the target.
#pragma once

#include <stdint.h>

namespace raster {

struct Target {
    uint16_t *rgb565 = nullptr;  // exactly one of these is set
    uint32_t *rgba = nullptr;
    int w = 0, h = 0;            // stride = w
};

// A source image: RGBA8888, straight alpha, `pitch` pixels per row.
struct Source {
    const uint32_t *px = nullptr;
    int w = 0, h = 0, pitch = 0;
};

// Where and how a source lands on the target.
struct Transform {
    float cx = 0, cy = 0;        // target point the source's centre lands on
    float scale = 1;             // target pixels per source pixel
    float rotation = 0;          // radians, clockwise on screen
    bool flipX = false;          // mirror the source left-right first
    uint8_t alpha = 255;         // overall opacity (ghost effect)
    int brightness = 0;          // -100..100 (brightness effect)
    float color = 0;             // colour effect (hue shift, 200 = full turn)
};

void blit(const Target &t, const Source &s, const Transform &x);

// Filled shapes in straight-alpha RGBA colour.
void fillRect(const Target &t, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void fillRoundRect(const Target &t, int x, int y, int w, int h, int radius, uint8_t r, uint8_t g, uint8_t b,
                   uint8_t a);
// A line of width 2*radius with round caps (the pen), anti-aliased edges.
void capsule(const Target &t, float x1, float y1, float x2, float y2, float radius, uint8_t r, uint8_t g, uint8_t b,
             uint8_t a);
// An upward/downward pointing triangle (speech bubble tail).
void fillTriangle(const Target &t, float x0, float y0, float x1, float y1, float x2, float y2, uint8_t r, uint8_t g,
                  uint8_t b, uint8_t a);

// Composite an RGBA layer (same size as the target or smaller, placed at
// ox,oy and scaled by `scale`) over an RGB565 target.
void compositeLayer(const Target &t, const uint32_t *layer, int lw, int lh, int ox, int oy, float scale);

// Text with the built-in 8x8 font (papp_text.cpp): UTF-8, '\n' breaks lines.
// `px` is the glyph cell height in target pixels (8 = 1:1).
void text(const Target &t, const char *utf8, int x, int y, float px, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
// Size of the text in target pixels at glyph cell height `px`.
void textSize(const char *utf8, float px, int *w, int *h);
// Line advance for glyph cell height `px`.
inline int lineHeight(float px) { return (int)(px * 1.25f + 0.5f); }

// Pack / blend helpers.
inline uint16_t pack565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// fg over bg with a 0..32 alpha, both RGB565.
inline uint16_t blend565(uint16_t bg, uint16_t fg, uint32_t a32)
{
    uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
    uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
    uint32_t res = ((((f - b) * a32) >> 5) + b) & 0x07E0F81Fu;
    return (uint16_t)((res >> 16) | res);
}

// Straight-alpha "over" into an RGBA8888 pixel.
inline uint32_t overRGBA(uint32_t d, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    const uint32_t da = d >> 24;
    if (a >= 255 || da == 0) {
        return r | (g << 8) | (b << 16) | (a << 24);
    }
    // outA = a + da * (1 - a); colours weighted by their share of outA.
    const uint32_t dw = da * (255 - a) / 255;
    const uint32_t oa = a + dw;
    const uint32_t dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
    const uint32_t orr = (r * a + dr * dw) / oa;
    const uint32_t og = (g * a + dg * dw) / oa;
    const uint32_t ob = (b * a + db * dw) / oa;
    return orr | (og << 8) | (ob << 16) | (oa << 24);
}

}  // namespace raster
