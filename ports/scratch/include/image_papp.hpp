// Costume images for the Scratch Everywhere! PAPP renderer: the RGBA pixels
// the runtime decodes (stb_image for bitmaps, LunaSVG for vectors) are drawn
// straight from memory by the software rasteriser; there is no separate
// texture. The pixels also serve the runtime's pixel-accurate collisions.
#pragma once
#include "nonstd/expected.hpp"
#include <image.hpp>
#include <se_export.hpp>

class SE_EXPORT Image_Papp : public Image {
  public:
    Image_Papp(std::string filePath, bool fromScratchProject = true, bool bitmapHalfQuality = false, float scale = 1);
    Image_Papp(std::string filePath, mz_zip_archive *zip, bool bitmapHalfQuality = false, float scale = 1);
    ~Image_Papp() override = default;

    void render(ImageRenderParams &params) override;
    void renderNineslice(double xPos, double yPos, double width, double height, double padding,
                         bool centered = false) override;

    void *getNativeTexture() override { return imgData.pixels; }

  protected:
    nonstd::expected<void, std::string> refreshTexture() override { return {}; }
};
