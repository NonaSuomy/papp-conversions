// Costume images for the Scratch Everywhere! PAPP renderer (include/
// image_papp.hpp): drawn from the decoded RGBA pixels by papp_raster.cpp.
#include "papp_raster.h"
#include "papp_render.h"

#include <image_papp.hpp>

#include <algorithm>
#include <cmath>

// Vector costumes are rasterised at the size they are shown (up to 5x, the
// runtime's limit); this caps one costume at 1024x1024 (4 MB of RGBA).
static const std::pair<unsigned int, unsigned int> MAX_TEXTURE = {1024, 1024};

Image_Papp::Image_Papp(std::string filePath, bool fromScratchProject, bool bitmapHalfQuality, float scale)
{
    maxTextureSize = MAX_TEXTURE;
    const auto result = init(filePath, fromScratchProject, bitmapHalfQuality, scale);
    if (!result.has_value()) {
        error = result.error();
    }
}

Image_Papp::Image_Papp(std::string filePath, mz_zip_archive *zip, bool bitmapHalfQuality, float scale)
{
    maxTextureSize = MAX_TEXTURE;
    const auto result = init(filePath, zip, bitmapHalfQuality, scale);
    if (!result.has_value()) {
        error = result.error();
    }
}

void Image_Papp::render(ImageRenderParams &params)
{
    freeTimer = maxFreeTimer;
    if (imgData.pixels == nullptr || imgData.width <= 0 || imgData.height <= 0) {
        return;
    }
    raster::Source src;
    const int pitch = imgData.pitch / 4;
    int sx = 0, sy = 0, sw = imgData.width, sh = imgData.height;
    if (params.subrect != nullptr) {
        sx = std::clamp(params.subrect->x, 0, imgData.width);
        sy = std::clamp(params.subrect->y, 0, imgData.height);
        sw = std::clamp(params.subrect->w, 0, imgData.width - sx);
        sh = std::clamp(params.subrect->h, 0, imgData.height - sy);
    }
    src.px = static_cast<const uint32_t *>(imgData.pixels) + sy * pitch + sx;
    src.w = sw;
    src.h = sh;
    src.pitch = pitch;

    // params.scale is relative to the image's own size (its pixels divided
    // by imgData.scale, the scale a vector costume was rasterised at).
    raster::Transform x;
    x.scale = params.scale / imgData.scale;
    const float dw = sw * x.scale, dh = sh * x.scale;
    x.cx = params.centered ? params.x : params.x + dw * 0.5f;
    x.cy = params.centered ? params.y : params.y + dh * 0.5f;
    x.rotation = params.rotation;
    x.flipX = params.flip;
    x.alpha = static_cast<uint8_t>(std::clamp(params.opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
    x.brightness = std::clamp(params.brightness, -100, 100);
    x.color = std::fmod(params.colorEffect, 200.0f);
    raster::blit(papp_render_target(), src, x);
}

// Nine-slice: corners unscaled, edges and centre stretched (nearest).
static void stretch(const raster::Target &t, const uint32_t *px, int pitch, int sx, int sy, int sw, int sh, int dx,
                    int dy, int dw, int dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    for (int y = 0; y < dh; y++) {
        const int ty = dy + y;
        if (ty < 0 || ty >= t.h) {
            continue;
        }
        const uint32_t *row = px + (sy + y * sh / dh) * pitch + sx;
        for (int x = 0; x < dw; x++) {
            const int tx = dx + x;
            if (tx < 0 || tx >= t.w) {
                continue;
            }
            const uint32_t p = row[x * sw / dw];
            const uint32_t a = p >> 24;
            if (a == 0) {
                continue;
            }
            const uint32_t r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
            if (t.rgb565 != nullptr) {
                uint16_t &d = t.rgb565[ty * t.w + tx];
                const uint16_t c = raster::pack565(r, g, b);
                d = a >= 250 ? c : raster::blend565(d, c, (a + 4) >> 3);
            } else {
                uint32_t &d = t.rgba[ty * t.w + tx];
                d = raster::overRGBA(d, r, g, b, a);
            }
        }
    }
}

void Image_Papp::renderNineslice(double xPos, double yPos, double width, double height, double padding, bool centered)
{
    freeTimer = maxFreeTimer;
    if (imgData.pixels == nullptr) {
        return;
    }
    const raster::Target &t = papp_render_target();
    const int x = static_cast<int>(xPos - (centered ? width / 2 : 0));
    const int y = static_cast<int>(yPos - (centered ? height / 2 : 0));
    const int w = static_cast<int>(width), h = static_cast<int>(height);
    const int iw = imgData.width, ih = imgData.height, pitch = imgData.pitch / 4;
    const int p = std::max(1, static_cast<int>(std::min({padding, iw / 2.0, ih / 2.0})));
    const int scw = std::max(0, iw - 2 * p), sch = std::max(0, ih - 2 * p);
    const int dcw = std::max(0, w - 2 * p), dch = std::max(0, h - 2 * p);
    const uint32_t *px = static_cast<const uint32_t *>(imgData.pixels);
    const int sxs[3] = {0, p, iw - p}, sws[3] = {p, scw, p};
    const int sys[3] = {0, p, ih - p}, shs[3] = {p, sch, p};
    const int dxs[3] = {x, x + p, x + p + dcw}, dws[3] = {p, dcw, p};
    const int dys[3] = {y, y + p, y + p + dch}, dhs[3] = {p, dch, p};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            stretch(t, px, pitch, sxs[col], sys[row], sws[col], shs[row], dxs[col], dys[row], dws[col], dhs[row]);
        }
    }
}
