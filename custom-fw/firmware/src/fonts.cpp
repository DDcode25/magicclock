#include "fonts.h"
#include "fonts/font_ua_15.h"
#include "fonts/font_ua_24b.h"
#include "fonts/city_fonts.h"
#include "fonts/font_digits_36b.h"
#include "fonts/font_digits_45b.h"
#include "fonts/font_digits_56b.h"
#include "fonts/font_digits_16b.h"
#include "fonts/font_num_24b.h"
#include "fonts/font_num_28b.h"
#include "fonts/font_digits_71b.h"
#include "fonts/font_alarm.h"
#include "fonts/font_price_52b.h"
#include "fonts/font_price_40b.h"
#include "fonts/font_price_28b.h"
#include "fonts/font_price_22b.h"

// VLW data currently loaded on the screen (nullptr = built-in fonts). Sprites load every time: they
// are short-lived and keep their own font.
static const uint8_t *loaded = nullptr;
static TFT_eSPI *screen = nullptr;

static void load(TFT_eSPI &tft, const uint8_t *data) {
  if (!screen) screen = &tft;  // ui::begin draws on the screen first
  bool isScreen = &tft == screen;
  if (isScreen && data == loaded) return;
  if (data) tft.loadFont(data);  // replaces any loaded font
  else tft.unloadFont();
  if (isScreen) loaded = data;
}

void useFont(TFT_eSPI &tft, UiFont font) {
  static const uint8_t *const DATA[] = {nullptr, FONT_UA_15, FONT_UA_24B, FONT_DIGITS_71B, FONT_DIGITS_36B, FONT_DIGITS_45B, FONT_DIGITS_56B, FONT_DIGITS_16B, FONT_NUM_24B, FONT_NUM_28B, FONT_ALARM_DATA,
                                        FONT_PRICE_52B, FONT_PRICE_40B, FONT_PRICE_28B, FONT_PRICE_22B};
  load(tft, DATA[font]);
}

void useCityFont(TFT_eSPI &tft, int region) {
  if (region >= 0 && region < (int)(sizeof CITY_FONT / sizeof CITY_FONT[0])) load(tft, CITY_FONT[region]);
  else useFont(tft, FONT_LARGE);
}
