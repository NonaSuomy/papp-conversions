// The PAPP renderer backend for Scratch Everywhere!: Render::* (include/
// render.hpp) drawn in software into a 480x360 RGB565 frame (papp_video.cpp
// shows it, scaled by the P4's PPA). Sprites are composited back to front
// with papp_raster.cpp; the pen layer is an RGBA8888 bitmap the size of the
// project's stage; speech bubbles and monitors use the built-in 8x8 font.
#include "papp_port.h"
#include "papp_raster.h"
#include "papp_render.h"

#include <blockExecutor.hpp>
#include <color.hpp>
#include <image.hpp>
#include <input.hpp>
#include <math.hpp>
#include <render.hpp>
#include <runtime.hpp>
#include <speech_manager.hpp>
#include <speech_text.hpp>
#include <sprite.hpp>
#include <text_papp.hpp>
#include <window.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

WindowSE *globalWindow = nullptr;

static raster::Target s_frame;         // the frame being built
static const raster::Target *s_target = &s_frame;
static long long s_last_frame_us = 0;  // when the runtime last started a frame (Input::getInput)

// ── Pen layer ────────────────────────────────────────────────────────────────
// Scratch::projectWidth x projectHeight RGBA pixels (straight alpha). Only
// the rows touched since the last clear are composited.

static uint32_t *s_pen = nullptr;
static int s_pen_w = 0, s_pen_h = 0;
static int s_pen_y0 = 0, s_pen_y1 = 0;  // touched rows [y0, y1)

static raster::Target pen_target()
{
    raster::Target t;
    t.rgba = s_pen;
    t.w = s_pen_w;
    t.h = s_pen_h;
    return t;
}

static void pen_touch(float y0, float y1)
{
    const int a = std::max(0, (int)std::floor(y0));
    const int b = std::min(s_pen_h, (int)std::ceil(y1) + 1);
    if (a >= b) {
        return;
    }
    if (s_pen_y0 >= s_pen_y1) {
        s_pen_y0 = a;
        s_pen_y1 = b;
    } else {
        s_pen_y0 = std::min(s_pen_y0, a);
        s_pen_y1 = std::max(s_pen_y1, b);
    }
}

// The stage's place in the frame: project pixels -> frame pixels.
static void stage_rect(float &scale, float &ox, float &oy)
{
    scale = std::min(static_cast<float>(PAPP_FRAME_W) / Scratch::projectWidth,
                     static_cast<float>(PAPP_FRAME_H) / Scratch::projectHeight);
    ox = (PAPP_FRAME_W - Scratch::projectWidth * scale) * 0.5f;
    oy = (PAPP_FRAME_H - Scratch::projectHeight * scale) * 0.5f;
}

static void pen_colour(Sprite *sprite, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a)
{
    const ColorRGBA c = CSBT2RGBA(sprite->penData.color);
    r = (uint8_t)std::clamp(c.r, 0.0f, 255.0f);
    g = (uint8_t)std::clamp(c.g, 0.0f, 255.0f);
    b = (uint8_t)std::clamp(c.b, 0.0f, 255.0f);
    a = (uint8_t)std::clamp((100.0 - sprite->penData.color.transparency) / 100.0 * 255.0, 0.0, 255.0);
}

static void pen_line(double x1, double y1, double x2, double y2, Sprite *sprite)
{
    if (s_pen == nullptr && !Render::initPen()) {
        return;
    }
    uint8_t r, g, b, a;
    pen_colour(sprite, r, g, b, a);
    const float px1 = (float)(x1 + Scratch::projectWidth / 2.0), py1 = (float)(Scratch::projectHeight / 2.0 - y1);
    const float px2 = (float)(x2 + Scratch::projectWidth / 2.0), py2 = (float)(Scratch::projectHeight / 2.0 - y2);
    const float radius = (float)(sprite->penData.size / 2.0);
    raster::capsule(pen_target(), px1, py1, px2, py2, radius, r, g, b, a);
    pen_touch(std::min(py1, py2) - radius - 1, std::max(py1, py2) + radius + 1);
}

bool Render::initPen()
{
    if (s_pen != nullptr && s_pen_w == Scratch::projectWidth && s_pen_h == Scratch::projectHeight) {
        return true;
    }
    free(s_pen);
    s_pen_w = std::max(1, Scratch::projectWidth);
    s_pen_h = std::max(1, Scratch::projectHeight);
    s_pen = static_cast<uint32_t *>(calloc((size_t)s_pen_w * s_pen_h, sizeof(uint32_t)));
    s_pen_y0 = s_pen_y1 = 0;
    if (s_pen == nullptr) {
        s_pen_w = s_pen_h = 0;
        return false;
    }
    return true;
}

void Render::penMoveFast(double x1, double y1, double x2, double y2, Sprite *sprite)
{
    pen_line(x1, y1, x2, y2, sprite);
}

void Render::penMoveAccurate(double x1, double y1, double x2, double y2, Sprite *sprite)
{
    pen_line(x1, y1, x2, y2, sprite);
}

void Render::penDotFast(Sprite *sprite)
{
    pen_line(sprite->xPosition, sprite->yPosition, sprite->xPosition, sprite->yPosition, sprite);
}

void Render::penDotAccurate(Sprite *sprite)
{
    pen_line(sprite->xPosition, sprite->yPosition, sprite->xPosition, sprite->yPosition, sprite);
}

void Render::penStamp(Sprite *sprite)
{
    if (s_pen == nullptr && !initPen()) {
        return;
    }
    const Costume &costume = sprite->costumes[sprite->currentCostume];
    auto imgFind = Scratch::costumeImages.find(costume.fullName);
    if (imgFind == Scratch::costumeImages.end()) {
        return;
    }
    calculateRenderPosition(sprite, costume.isSVG);
    // Frame coordinates -> pen layer coordinates.
    float scale, ox, oy;
    stage_rect(scale, ox, oy);
    ImageRenderParams params;
    params.centered = true;
    params.x = (sprite->renderInfo.renderX - ox) / scale;
    params.y = (sprite->renderInfo.renderY - oy) / scale;
    params.rotation = sprite->renderInfo.renderRotation;
    params.scale = sprite->renderInfo.renderScaleY / scale;
    params.flip = (sprite->rotationStyle == sprite->LEFT_RIGHT && sprite->rotation < 0);
    params.opacity = 1.0f - (std::clamp(sprite->ghostEffect, 0.0f, 100.0f) * 0.01f);
    params.brightness = sprite->brightnessEffect;
    params.colorEffect = sprite->colorEffect;
    const raster::Target pen = pen_target();
    s_target = &pen;
    imgFind->second->render(params);
    s_target = &s_frame;
    const float reach = std::hypot((float)imgFind->second->getWidth(), (float)imgFind->second->getHeight()) *
                        params.scale;
    pen_touch(params.y - reach, params.y + reach);
}

void Render::penClear()
{
    if (s_pen != nullptr) {
        memset(s_pen, 0, (size_t)s_pen_w * s_pen_h * sizeof(uint32_t));
    }
    s_pen_y0 = s_pen_y1 = 0;
}

void Render::renderPenLayer()
{
    if (s_pen == nullptr || s_pen_y0 >= s_pen_y1) {
        return;
    }
    float scale, ox, oy;
    stage_rect(scale, ox, oy);
    // Only the touched rows: a band of the layer placed at its frame position.
    const int rows = s_pen_y1 - s_pen_y0;
    raster::compositeLayer(s_frame, s_pen + (size_t)s_pen_y0 * s_pen_w, s_pen_w, rows, (int)(ox + 0.5f),
                           (int)(oy + s_pen_y0 * scale + 0.5f), scale);
}

// ── Speech bubbles ───────────────────────────────────────────────────────────

class SpeechTextPapp : public TextObjectPapp, public SpeechText {
  public:
    SpeechTextPapp(const std::string &text, int maxWidth) : TextObjectPapp(text, 0, 0), SpeechText(text, maxWidth)
    {
        setColor(Math::color(0x57, 0x5E, 0x75, 255));
        setCenterAligned(false);
        setScale(0.5f);
        platformSetText(wrapText());
    }

    void setText(std::string txt) override { SpeechText::setText(txt); }

  protected:
    float measureTextWidth(const std::string &text) override { return getStringSize(text)[0]; }
    void platformSetText(const std::string &text) override { TextObjectPapp::setText(text); }
};

class SpeechManagerPapp : public SpeechManager {
  protected:
    double getCurrentTime() override { return papp_time_us() / 1000000.0; }

    void createSpeechObject(Sprite *sprite, const std::string &message) override
    {
        speechObjects[sprite] = std::make_unique<SpeechTextPapp>(message, 170);
    }

  public:
    ~SpeechManagerPapp() override { cleanup(); }

    // Scratch's bubble: white, a light grey outline and a tail towards the
    // sprite (a triangle for "say", two dots for "think"), beside the
    // sprite's top corner on the side towards the stage centre.
    void render(int offsetX, int offsetY) override
    {
        (void)offsetX;
        (void)offsetY;
        float scale, ox, oy;
        stage_rect(scale, ox, oy);
        for (auto &[sprite, obj] : speechObjects) {
            if (!obj || !sprite->visible) {
                continue;
            }
            const int cx = (int)(ox + (sprite->xPosition + Scratch::projectWidth / 2.0) * scale);
            const int cy = (int)(oy + (Scratch::projectHeight / 2.0 - sprite->yPosition) * scale);
            const int sw = (int)(sprite->spriteWidth * sprite->size / 100.0 * scale);
            const int sh = (int)(sprite->spriteHeight * sprite->size / 100.0 * scale);
            const int top = cy - sh / 2;
            const bool rightSide = cx < PAPP_FRAME_W / 2;
            const std::vector<float> size = obj->getSize();
            const int tw = (int)size[0], th = (int)size[1];
            const int pad = 6;
            const int bw = tw + pad * 2, bh = th + pad * 2;
            int bx = rightSide ? cx + sw / 2 : cx - sw / 2 - bw;
            int by = top - 12 - bh;
            bx = std::clamp(bx, 0, PAPP_FRAME_W - bw);
            by = std::clamp(by, 0, PAPP_FRAME_H - bh - 12);
            const raster::Target &t = *s_target;
            raster::fillRoundRect(t, bx - 1, by - 1, bw + 2, bh + 2, 9, 0xC0, 0xC4, 0xCC, 255);
            raster::fillRoundRect(t, bx, by, bw, bh, 8, 255, 255, 255, 255);
            const auto styleIt = speechStyles.find(sprite);
            const bool think = styleIt != speechStyles.end() && styleIt->second == "think";
            const float tailX = rightSide ? bx + 14.0f : bx + bw - 14.0f;
            const float tipX = rightSide ? tailX - 6.0f : tailX + 6.0f;
            if (think) {
                raster::fillRoundRect(t, (int)tipX - 3, by + bh + 2, 7, 7, 3, 0xC0, 0xC4, 0xCC, 255);
                raster::fillRoundRect(t, (int)tipX - 2, by + bh + 3, 5, 5, 2, 255, 255, 255, 255);
            } else {
                raster::fillTriangle(t, tailX - 6, (float)by + bh - 1, tailX + 6, (float)by + bh - 1, tipX,
                                     (float)by + bh + 9, 255, 255, 255, 255);
            }
            obj->render(bx + pad, by + pad);
        }
    }
};

static SpeechManagerPapp *s_speech = nullptr;

bool Render::createSpeechManager()
{
    if (s_speech == nullptr) {
        s_speech = new SpeechManagerPapp();
    }
    return s_speech != nullptr;
}

SpeechManager *Render::getSpeechManager()
{
    return s_speech;
}

void Render::destroySpeechManager()
{
    delete s_speech;
    s_speech = nullptr;
}

// ── Frames ───────────────────────────────────────────────────────────────────

const raster::Target &papp_render_target()
{
    return *s_target;
}

const raster::Target &papp_frame_begin(uint8_t r, uint8_t g, uint8_t b)
{
    s_frame.rgb565 = papp_video_back();
    s_frame.w = PAPP_FRAME_W;
    s_frame.h = PAPP_FRAME_H;
    s_target = &s_frame;
    const uint16_t c = raster::pack565(r, g, b);
    if (c == 0xFFFF || c == 0) {
        memset(s_frame.rgb565, c & 0xFF, PAPP_FRAME_W * PAPP_FRAME_H * sizeof(uint16_t));
    } else {
        for (int i = 0; i < PAPP_FRAME_W * PAPP_FRAME_H; i++) {
            s_frame.rgb565[i] = c;
        }
    }
    return s_frame;
}

// Sleep until `due` (microseconds): whole ticks first, then yields.
static void wait_until(long long due)
{
    for (;;) {
        const long long left = due - papp_time_us();
        if (left <= 0) {
            return;
        }
        if (left >= 10000) {
            papp_svc->delay_ms((int)(left / 10000) * 10);
        } else {
            papp_svc->delay_ms(1);  // below one tick: a yield
        }
    }
}

void papp_frame_end(int fps)
{
    papp_video_present();
    papp_audio_pump();
    if (fps > 0) {
        // The app's own screens: fps frames a second.
        static long long last = 0;
        wait_until(last + 1000000LL / fps);
        last = papp_time_us();
    }
    papp_yield_maybe();
}

void papp_render_mark_frame()
{
    s_last_frame_us = papp_time_us();
}

bool Render::Init()
{
    return papp_video_init() == 0;
}

void Render::deInit()
{
    destroySpeechManager();
    free(s_pen);
    s_pen = nullptr;
    s_pen_w = s_pen_h = 0;
}

void *Render::getRenderer()
{
    return nullptr;
}

void Render::setRenderTarget(void *renderTarget)
{
    (void)renderTarget;  // render-to-texture is only used by the upstream menus
}

void Render::clearRenderTarget()
{
    s_target = &s_frame;
}

int Render::getWidth()
{
    return PAPP_FRAME_W;
}

int Render::getHeight()
{
    return PAPP_FRAME_H;
}

float Render::getPixelDensity()
{
    return 1.0f;
}

void Render::beginFrame(int screen, int colorR, int colorG, int colorB)
{
    (void)screen;
    if (!hasFrameBegan) {
        papp_frame_begin((uint8_t)colorR, (uint8_t)colorG, (uint8_t)colorB);
        hasFrameBegan = true;
    }
}

void Render::endFrame(bool shouldFlush)
{
    (void)shouldFlush;
    papp_frame_end(30);
    hasFrameBegan = false;
}

void Render::drawBox(int w, int h, int x, int y, uint8_t colorR, uint8_t colorG, uint8_t colorB, uint8_t colorA)
{
    raster::fillRect(*s_target, x - w / 2, y - h / 2, w, h, colorR, colorG, colorB, colorA);
}

static void draw_black_bars()
{
    float scale, ox, oy;
    stage_rect(scale, ox, oy);
    if (ox >= 1.0f) {
        const int bar = (int)std::ceil(ox);
        raster::fillRect(s_frame, 0, 0, bar, PAPP_FRAME_H, 0, 0, 0, 255);
        raster::fillRect(s_frame, PAPP_FRAME_W - bar, 0, bar, PAPP_FRAME_H, 0, 0, 0, 255);
    }
    if (oy >= 1.0f) {
        const int bar = (int)std::ceil(oy);
        raster::fillRect(s_frame, 0, 0, PAPP_FRAME_W, bar, 0, 0, 0, 255);
        raster::fillRect(s_frame, 0, PAPP_FRAME_H - bar, PAPP_FRAME_W, bar, 0, 0, 0, 255);
    }
}

void papp_draw_stage()
{
    papp_frame_begin(255, 255, 255);
    for (auto it = Scratch::sprites.rbegin(); it != Scratch::sprites.rend(); ++it) {
        Sprite *sprite = *it;
        if (!sprite->costumes.empty()) {
            const Costume &costume = sprite->costumes[sprite->currentCostume];
            auto imgFind = Scratch::costumeImages.find(costume.fullName);
            if (imgFind != Scratch::costumeImages.end()) {
                Render::calculateRenderPosition(sprite, costume.isSVG);
                if (sprite->visible) {
                    ImageRenderParams params;
                    params.centered = true;
                    params.x = sprite->renderInfo.renderX;
                    params.y = sprite->renderInfo.renderY;
                    params.rotation = sprite->renderInfo.renderRotation;
                    params.scale = sprite->renderInfo.renderScaleY;
                    params.flip = (sprite->rotationStyle == sprite->LEFT_RIGHT && sprite->rotation < 0);
                    params.opacity = 1.0f - (std::clamp(sprite->ghostEffect, 0.0f, 100.0f) * 0.01f);
                    params.brightness = sprite->brightnessEffect;
                    params.colorEffect = sprite->colorEffect;
                    imgFind->second->render(params);
                }
            }
        }
        if (sprite->isStage) {
            Render::renderPenLayer();
        }
    }
    if (s_speech != nullptr) {
        s_speech->render(0, 0);
    }
    draw_black_bars();
    Render::renderMonitors();
}

void Render::renderSprites()
{
    papp_draw_stage();
    papp_draw_mouse_pointer();
    papp_frame_end(0);  // the runtime paces frames (Render::checkFramerate)
}

// Called at the top of every runtime step (Scratch::stepScratchProject). The
// runtime only reads input and draws when a frame is due (checkFramerate)
// but otherwise keeps running scripts. Like Scratch's own sequencer (which
// works for 3/4 of a frame), scripts get a budget of the frame, and none
// when they wait for the next redraw or there are none: then the app task
// sleeps until the next frame is due. The budget leaves at least one
// FreeRTOS tick (10 ms) of real sleep per frame for the other tasks.
bool Render::appShouldRun()
{
    if (OS::toExit) {
        return false;
    }
    static long long last_poll = 0;
    const long long now = papp_time_us();
    if (now - last_poll > 50000) {
        // Scripts running for a long time without a frame: still notice a quit.
        papp_input_poll();
        last_poll = now;
    }
    if (papp_input.quit) {
        OS::toExit = true;
        return false;
    }
    if (!Scratch::turbo && Scratch::FPS > 0) {
        const long long frame = 1000000LL / Scratch::FPS;
        const long long due = s_last_frame_us + frame;
        const long long budget = std::max(frame / 2, frame - 12000);
        if (Scratch::forceRedraw || BlockExecutor::threads.empty() || now - s_last_frame_us > budget) {
            wait_until(due);
        }
    }
    papp_yield_maybe();
    return true;
}
