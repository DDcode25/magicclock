#include "fonts.h"
#include "fonts/font_ua_15.h"
#include "fonts/font_ua_24b.h"
#include "fonts/city_fonts.h"
#include "fonts/font_digits_36b.h"
#include "fonts/font_digits_71b.h"

static const uint8_t *loaded = nullptr;  // VLW data currently loaded, nullptr = built-in fonts

static void load(TFT_eSPI &tft, const uint8_t *data) {
  if (data == loaded) return;
  if (data) tft.loadFont(data);  // replaces any loaded font
  else tft.unloadFont();
  loaded = data;
}

void useFont(TFT_eSPI &tft, UiFont font) {
  static const uint8_t *const DATA[] = {nullptr, FONT_UA_15, FONT_UA_24B, FONT_DIGITS_71B, FONT_DIGITS_36B};
  load(tft, DATA[font]);
}

void useCityFont(TFT_eSPI &tft, int region) {
  load(tft, region >= 0 && region < (int)(sizeof CITY_FONT / sizeof CITY_FONT[0]) ? CITY_FONT[region] : CLOCK_FONT);
}
