#include "fonts.h"
#include "fonts/font_ua_15.h"
#include "fonts/font_ua_24b.h"
#include "fonts/font_ua_30b.h"
#include "fonts/font_digits_46b.h"

void useFont(TFT_eSPI &tft, UiFont font) {
  static UiFont loaded = FONT_BUILTIN;
  if (font == loaded) return;
  if (font == FONT_BUILTIN) tft.unloadFont();
  else {
    static const uint8_t *const DATA[] = {nullptr, FONT_UA_15, FONT_UA_24B, FONT_UA_30B, FONT_DIGITS_46B};
    tft.loadFont(DATA[font]);  // replaces any loaded font
  }
  loaded = font;
}
