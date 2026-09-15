// Software rasteriser for the Scratch Everywhere! PAPP renderer (see
// papp_raster.h). Costumes are sampled nearest-neighbour with 16.16 fixed
// point stepping along each row; the row is first narrowed to where it
// crosses the (rotated, scaled) source so the inner loop only samples.
// Effects follow Scratch's sprite shader (scratch-render sprite.frag):
// colour (hue shift, with its minimum saturation / lightness), brightness
// (added to each channel) and ghost (opacity).
#include "papp_raster.h"

#include <math.h>
#include <string.h>

namespace raster {

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline uint32_t clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint32_t)v); }

// Scratch's colour effect: hue shifted by `shift` turns in HSV space.
static inline void hueShift(uint32_t &r, uint32_t &g, uint32_t &b, float shift)
{
    const float rf = r * (1.0f / 255), gf = g * (1.0f / 255), bf = b * (1.0f / 255);
    const float mx = fmaxf(rf, fmaxf(gf, bf));
    const float mn = fminf(rf, fminf(gf, bf));
    const float d = mx - mn;
    float h = 0, s = mx > 0 ? d / mx : 0, v = mx;
    if (d > 0) {
        if (mx == rf) {
            h = (gf - bf) / d;
        } else if (mx == gf) {
            h = 2 + (bf - rf) / d;
        } else {
            h = 4 + (rf - gf) / d;
        }
        h *= 1.0f / 6;
        if (h < 0) {
            h += 1;
        }
    }
    const float minLightness = 0.11f / 2, minSaturation = 0.09f;
    if (v < minLightness) {
        h = 0;
        s = 1;
        v = minLightness;
    } else if (s < minSaturation) {
        h = 0;
        s = minSaturation;
    }
    h = fmodf(h + shift, 1.0f);
    if (h < 0) {
        h += 1;
    }
    const float h6 = h * 6;
    const int i = (int)h6;
    const float f = h6 - i;
    const float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    float ro, go, bo;
    switch (i % 6) {
    case 0: ro = v; go = t; bo = p; break;
    case 1: ro = q; go = v; bo = p; break;
    case 2: ro = p; go = v; bo = t; break;
    case 3: ro = p; go = q; bo = v; break;
    case 4: ro = t; go = p; bo = v; break;
    default: ro = v; go = p; bo = q; break;
    }
    r = (uint32_t)(ro * 255 + 0.5f);
    g = (uint32_t)(go * 255 + 0.5f);
    b = (uint32_t)(bo * 255 + 0.5f);
}

// Narrow [tmin, tmax) (pixels along a row) to where p0 + d * t lies in [0, lim).
static inline void clipAxis(float p0, float d, float lim, float &tmin, float &tmax)
{
    if (d > -1e-6f && d < 1e-6f) {
        if (p0 < 0 || p0 >= lim) {
            tmax = tmin - 1;
        }
        return;
    }
    float t0 = (0 - p0) / d;
    float t1 = (lim - p0) / d;
    if (t0 > t1) {
        const float s = t0;
        t0 = t1;
        t1 = s;
    }
    if (t0 > tmin) {
        tmin = t0;
    }
    if (t1 < tmax) {
        tmax = t1;
    }
}

// One source pixel through the effects: false when fully transparent.
static inline bool shade(uint32_t p, const Transform &x, bool plain, uint32_t &r, uint32_t &g, uint32_t &b,
                         uint32_t &a)
{
    a = p >> 24;
    if (a == 0) {
        return false;
    }
    r = p & 0xFF;
    g = (p >> 8) & 0xFF;
    b = (p >> 16) & 0xFF;
    if (!plain) {
        if (x.color != 0) {
            hueShift(r, g, b, x.color * (1.0f / 200));
        }
        if (x.brightness != 0) {
            const int add = x.brightness * 255 / 100;
            r = clamp255((int)r + add);
            g = clamp255((int)g + add);
            b = clamp255((int)b + add);
        }
        a = a * x.alpha / 255;
        if (a == 0) {
            return false;
        }
    }
    return true;
}

void blit(const Target &t, const Source &s, const Transform &x)
{
    if (s.px == nullptr || s.w <= 0 || s.h <= 0 || !(x.scale > 0) || x.alpha == 0) {
        return;
    }
    const float c = cosf(x.rotation), sn = sinf(x.rotation);
    const float hw = s.w * 0.5f * x.scale, hh = s.h * 0.5f * x.scale;
    const float ex = fabsf(c) * hw + fabsf(sn) * hh;
    const float ey = fabsf(sn) * hw + fabsf(c) * hh;
    const int x0 = imax(0, (int)floorf(x.cx - ex));
    const int x1 = imin(t.w, (int)ceilf(x.cx + ex));
    const int y0 = imax(0, (int)floorf(x.cy - ey));
    const int y1 = imin(t.h, (int)ceilf(x.cy + ey));
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    // Source coordinates of a target pixel centre (X, Y relative to cx, cy):
    //   u = ( X cos + Y sin) / scale + w/2,  v = (-X sin + Y cos) / scale + h/2
    const float inv = 1.0f / x.scale;
    float dudx = c * inv, dudy = sn * inv;
    const float dvdx = -sn * inv, dvdy = c * inv;
    if (x.flipX) {
        dudx = -dudx;
        dudy = -dudy;
    }
    const float uo = s.w * 0.5f, vo = s.h * 0.5f;
    const int32_t DU = (int32_t)(dudx * 65536.0f);
    const int32_t DV = (int32_t)(dvdx * 65536.0f);
    const bool plain = x.alpha == 255 && x.brightness == 0 && x.color == 0;
    const unsigned W = (unsigned)s.w, H = (unsigned)s.h;

    for (int py = y0; py < y1; py++) {
        const float Y = py + 0.5f - x.cy;
        const float X = x0 + 0.5f - x.cx;
        const float u = X * dudx + Y * dudy + uo;
        const float v = X * dvdx + Y * dvdy + vo;
        float tmin = 0, tmax = (float)(x1 - x0);
        clipAxis(u, dudx, (float)s.w, tmin, tmax);
        clipAxis(v, dvdx, (float)s.h, tmin, tmax);
        if (!(tmin < tmax)) {
            continue;
        }
        const int start = imax(0, (int)floorf(tmin));
        const int end = imin(x1 - x0, (int)ceilf(tmax));
        int32_t U = (int32_t)((u + dudx * start) * 65536.0f);
        int32_t V = (int32_t)((v + dvdx * start) * 65536.0f);
        if (t.rgb565 != nullptr) {
            uint16_t *dst = t.rgb565 + py * t.w + x0;
            for (int i = start; i < end; i++, U += DU, V += DV) {
                const int iu = U >> 16, iv = V >> 16;
                if ((unsigned)iu >= W || (unsigned)iv >= H) {
                    continue;
                }
                uint32_t r, g, b, a;
                if (!shade(s.px[iv * s.pitch + iu], x, plain, r, g, b, a)) {
                    continue;
                }
                const uint16_t col = pack565(r, g, b);
                dst[i] = a >= 250 ? col : blend565(dst[i], col, (a + 4) >> 3);
            }
        } else {
            uint32_t *dst = t.rgba + py * t.w + x0;
            for (int i = start; i < end; i++, U += DU, V += DV) {
                const int iu = U >> 16, iv = V >> 16;
                if ((unsigned)iu >= W || (unsigned)iv >= H) {
                    continue;
                }
                uint32_t r, g, b, a;
                if (!shade(s.px[iv * s.pitch + iu], x, plain, r, g, b, a)) {
                    continue;
                }
                dst[i] = overRGBA(dst[i], r, g, b, a);
            }
        }
    }
}

static inline void plot(const Target &t, int px, int py, uint32_t r, uint32_t g, uint32_t b, uint32_t a,
                        uint16_t col565)
{
    if (t.rgb565 != nullptr) {
        uint16_t &d = t.rgb565[py * t.w + px];
        d = a >= 250 ? col565 : blend565(d, col565, (a + 4) >> 3);
    } else {
        uint32_t &d = t.rgba[py * t.w + px];
        d = overRGBA(d, r, g, b, a);
    }
}

static void span(const Target &t, int py, int xa, int xb, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    xa = imax(xa, 0);
    xb = imin(xb, t.w);
    if (py < 0 || py >= t.h || xa >= xb || a == 0) {
        return;
    }
    const uint16_t col = pack565(r, g, b);
    if (t.rgb565 != nullptr && a >= 250) {
        uint16_t *d = t.rgb565 + py * t.w;
        for (int px = xa; px < xb; px++) {
            d[px] = col;
        }
        return;
    }
    for (int px = xa; px < xb; px++) {
        plot(t, px, py, r, g, b, a, col);
    }
}

void fillRect(const Target &t, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const int ya = imax(y, 0), yb = imin(y + h, t.h);
    for (int py = ya; py < yb; py++) {
        span(t, py, x, x + w, r, g, b, a);
    }
}

void fillRoundRect(const Target &t, int x, int y, int w, int h, int radius, uint8_t r, uint8_t g, uint8_t b,
                   uint8_t a)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    radius = imax(0, imin(radius, imin(w, h) / 2));
    for (int row = 0; row < h; row++) {
        int inset = 0;
        const int fromEdge = row < radius ? radius - row : (row >= h - radius ? row - (h - radius) + 1 : 0);
        if (fromEdge > 0) {
            const float dy = fromEdge - 0.5f;
            inset = (int)(radius - sqrtf(fmaxf(0.0f, (float)(radius * radius) - dy * dy)) + 0.5f);
        }
        span(t, y + row, x + inset, x + w - inset, r, g, b, a);
    }
}

void capsule(const Target &t, float x1, float y1, float x2, float y2, float radius, uint8_t r, uint8_t g, uint8_t b,
             uint8_t a)
{
    if (a == 0) {
        return;
    }
    if (radius < 0.5f) {
        radius = 0.5f;
    }
    const float reach = radius + 0.5f;
    const int bx0 = imax(0, (int)floorf(fminf(x1, x2) - reach));
    const int bx1 = imin(t.w, (int)ceilf(fmaxf(x1, x2) + reach));
    const int by0 = imax(0, (int)floorf(fminf(y1, y2) - reach));
    const int by1 = imin(t.h, (int)ceilf(fmaxf(y1, y2) + reach));
    const float dx = x2 - x1, dy = y2 - y1;
    const float len2 = dx * dx + dy * dy;
    const float inv = len2 > 0 ? 1.0f / len2 : 0;
    const float reach2 = reach * reach;
    const float inner = radius - 0.5f;
    const float inner2 = inner > 0 ? inner * inner : -1;
    const uint16_t col = pack565(r, g, b);
    for (int py = by0; py < by1; py++) {
        const float qy = py + 0.5f - y1;
        for (int px = bx0; px < bx1; px++) {
            const float qx = px + 0.5f - x1;
            float s = (qx * dx + qy * dy) * inv;
            s = s < 0 ? 0 : (s > 1 ? 1 : s);
            const float ex = qx - s * dx, ey = qy - s * dy;
            const float d2 = ex * ex + ey * ey;
            if (d2 >= reach2) {
                continue;
            }
            uint32_t aa = a;
            if (d2 > inner2) {
                const float cov = reach - sqrtf(d2);
                aa = cov >= 1 ? a : (uint32_t)(a * cov);
                if (aa == 0) {
                    continue;
                }
            }
            plot(t, px, py, r, g, b, aa, col);
        }
    }
}

void fillTriangle(const Target &t, float x0, float y0, float x1, float y1, float x2, float y2, uint8_t r, uint8_t g,
                  uint8_t b, uint8_t a)
{
    const int bx0 = imax(0, (int)floorf(fminf(x0, fminf(x1, x2))));
    const int bx1 = imin(t.w, (int)ceilf(fmaxf(x0, fmaxf(x1, x2))));
    const int by0 = imax(0, (int)floorf(fminf(y0, fminf(y1, y2))));
    const int by1 = imin(t.h, (int)ceilf(fmaxf(y0, fmaxf(y1, y2))));
    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) {
        return;
    }
    const uint16_t col = pack565(r, g, b);
    for (int py = by0; py < by1; py++) {
        for (int px = bx0; px < bx1; px++) {
            const float qx = px + 0.5f, qy = py + 0.5f;
            const float w0 = (x1 - qx) * (y2 - qy) - (x2 - qx) * (y1 - qy);
            const float w1 = (x2 - qx) * (y0 - qy) - (x0 - qx) * (y2 - qy);
            const float w2 = (x0 - qx) * (y1 - qy) - (x1 - qx) * (y0 - qy);
            if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) || (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                plot(t, px, py, r, g, b, a, col);
            }
        }
    }
}

void compositeLayer(const Target &t, const uint32_t *layer, int lw, int lh, int ox, int oy, float scale)
{
    if (layer == nullptr || t.rgb565 == nullptr || !(scale > 0)) {
        return;
    }
    const int tw = imin(t.w - ox, (int)(lw * scale));
    const int th = imin(t.h - oy, (int)(lh * scale));
    const float inv = 1.0f / scale;
    for (int y = imax(0, -oy); y < th; y++) {
        const int ly = imin(lh - 1, (int)(y * inv));
        const uint32_t *src = layer + ly * lw;
        uint16_t *dst = t.rgb565 + (y + oy) * t.w + ox;
        for (int x = imax(0, -ox); x < tw; x++) {
            const uint32_t p = src[scale == 1.0f ? x : imin(lw - 1, (int)(x * inv))];
            const uint32_t a = p >> 24;
            if (a == 0) {
                continue;
            }
            const uint16_t col = pack565(p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF);
            dst[x] = a >= 250 ? col : blend565(dst[x], col, (a + 4) >> 3);
        }
    }
}

}  // namespace raster
