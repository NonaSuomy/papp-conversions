// The 8x8 glyphs of font8x8 (Daniel Hepper, public domain; fetched at a
// pinned commit by the build): basic Latin and Latin-1. The headers define
// their tables at file scope as plain `char`, so they are compiled here, in
// C, once.
#include <stdint.h>

#include "font8x8_basic.h"
#include "font8x8_ext_latin.h"

const char *papp_font_glyph(uint32_t cp)
{
    if (cp < 128) {
        return font8x8_basic[cp];
    }
    if (cp >= 0xA0 && cp <= 0xFF) {
        return font8x8_ext_latin[cp - 0xA0];
    }
    return font8x8_basic['?'];
}
