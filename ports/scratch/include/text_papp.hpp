// Text for the Scratch Everywhere! PAPP renderer: the built-in 8x8 bitmap
// font (font8x8, public domain), drawn by the software rasteriser. Used for
// variable/list monitors, speech bubbles, "ask" prompts and the app's own
// screens. SVG costume text is rendered by LunaSVG instead.
#pragma once
#include <se_export.hpp>
#include <text.hpp>

class SE_EXPORT TextObjectPapp : public TextObject {
  public:
    // Glyph cell height in pixels at scale 1 (the upstream renderers use a
    // 30 px TrueType font there; monitors draw at scale 0.5).
    static constexpr float BASE_PX = 16.0f;

    TextObjectPapp(std::string txt, double posX, double posY, std::string fontPath = "");
    ~TextObjectPapp() override = default;

    void setText(std::string txt) override;
    void render(int xPos, int yPos) override;
    std::vector<float> getSize() override;
    std::vector<float> getStringSize(const std::string &txt) override;

    static void cleanupText() {}
};
