#include "fonts.h"
#include "fonts/font_ua_15.h"
#include "fonts/font_ua_24b.h"

void useFont(TFT_eSPI &tft, UiFont font) {
  static UiFont loaded = FONT_BUILTIN;
  if (font == loaded) return;
  if (font == FONT_BUILTIN) tft.unloadFont();
  else tft.loadFont(font == FONT_LARGE ? FONT_UA_24B : FONT_UA_15);  // replaces any loaded font
  loaded = font;
}
