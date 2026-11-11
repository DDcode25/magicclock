#include "weather.h"
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <time.h>
#include "apps.h"
#include "diag.h"
#include "fonts.h"
#include "net.h"
#include "store.h"
#include "widgets.h"

namespace weather {

static const char CITY[] PROGMEM = "/.sys/city.json";
static const char UNIT[] PROGMEM = "/.sys/unit.json";

struct Config {
  String city, nickname;
  int intervalMin = 20;
  bool fahrenheit = false;
  String wind = "m/s", pressure = "hPa";
};

struct Data {
  bool valid = false;
  String place;  // geocoded name (Ukrainian)
  float temp = 0, feels = 0, tmax = 0, tmin = 0, wind = 0, pressureHpa = 0;
  int humidity = 0, code = 0;
  bool day = true;
  time_t updated = 0;
};

static Config cfg;
static Data data;
static uint32_t version = 0;
static char error[48] = "";
static volatile bool fetchNow = true;
static volatile bool paused = false;

// Geocoder cache: only re-resolve when the city setting changes.
static String geoCity, geoName;
static float lat = 0, lon = 0;

static void loadConfig() {
  Config c;
  c.city = store::getStr(CITY, PSTR("cd1"), PSTR(""));
  c.nickname = store::getStr(CITY, PSTR("nickname"), PSTR(""));
  c.intervalMin = max(5, store::getInt(CITY, PSTR("w_i"), 20));
  c.fahrenheit = store::getStr(UNIT, PSTR("t_u"), PSTR("°C")) == "°F";
  c.wind = store::getStr(UNIT, PSTR("w_u"), PSTR("m/s"));
  c.pressure = store::getStr(UNIT, PSTR("p_u"), PSTR("hPa"));
  cfg = c;
}

static String urlEncode(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') out += c;
    else {
      char b[4];
      snprintf(b, sizeof b, "%%%02X", (uint8_t)c);
      out += b;
    }
  }
  return out;
}

static bool getJson(const String &url, JsonDocument &doc, String &err) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);  // no chunked encoding: the JSON is parsed straight from the stream
  if (!http.begin(client, url)) return err = F("немає з’єднання"), false;
  int code = http.GET();
  if (code != 200) {
    err = F("помилка HTTP ") + String(code);
    http.end();
    return false;
  }
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e) return err = F("погані дані"), false;
  return true;
}

static bool geocode(const Config &c, String &err) {
  if (c.city == geoCity && geoName.length()) return true;
  if (c.city.isEmpty()) return err = F("вкажіть місто"), false;
  bool numeric = true;
  for (size_t i = 0; i < c.city.length(); i++) numeric &= isDigit(c.city[i]);
  if (numeric) return err = F("потрібна назва міста"), false;  // OpenWeatherMap city ids need an OWM key
  JsonDocument doc;
  if (!getJson(String(F("http://geocoding-api.open-meteo.com/v1/search?count=1&language=uk&format=json&name=")) + urlEncode(c.city),
               doc, err))
    return false;
  JsonObject r = doc[F("results")][0];
  if (r.isNull()) return err = F("місто не знайдено"), false;
  lat = r[F("latitude")], lon = r[F("longitude")];
  geoName = r[F("name")] | c.city.c_str();
  geoCity = c.city;
  return true;
}

static bool fetch(const Config &c, Data &d, String &err) {
  if (!geocode(c, err)) return false;
  String url = String(F("http://api.open-meteo.com/v1/forecast?latitude=")) + String(lat, 4) + F("&longitude=") + String(lon, 4) +
               F("&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,"
                 "pressure_msl,is_day&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto"
                 "&wind_speed_unit=") +
               (c.wind.equals(F("km/h")) ? F("kmh") : c.wind.equals(F("mile/h")) ? F("mph") : F("ms")) +
               (c.fahrenheit ? String(F("&temperature_unit=fahrenheit")) : String());
  JsonDocument doc;
  if (!getJson(url, doc, err)) return false;
  JsonObject cur = doc[F("current")];
  if (cur.isNull()) return err = F("погані дані"), false;
  d.place = geoName;
  d.temp = cur[F("temperature_2m")];
  d.feels = cur[F("apparent_temperature")];
  d.humidity = cur[F("relative_humidity_2m")];
  d.code = cur[F("weather_code")];
  d.wind = cur[F("wind_speed_10m")];
  d.pressureHpa = cur[F("pressure_msl")];
  d.day = (cur[F("is_day")] | 1) == 1;
  d.tmax = doc[F("daily")][F("temperature_2m_max")][0];
  d.tmin = doc[F("daily")][F("temperature_2m_min")][0];
  d.updated = time(nullptr);
  d.valid = true;
  return true;
}

static uint32_t last = 0;
static bool lastOk = false;
bool poll() {
  if (paused || !settings.appEnabled[APP_WEATHER] || !net::connected()) return false;
  Config c;
  c = cfg;
  uint32_t period = lastOk ? c.intervalMin * 60000UL : 60000UL;  // retry errors every minute
  if (!fetchNow && last && millis() - last < period) return false;
  fetchNow = false;
  last = millis();

  Data d;
  String err;
  net::fetchLock();
  diag::crumb(diag::FETCH, PSTR("weather fetch"));
  bool ok = fetch(c, d, err);
  net::fetchUnlock();
  lastOk = ok;
  Serial.printf_P(PSTR("weather: %s\n"), ok ? "ok" : err.c_str());
  if (ok) {
    data = d;
    version++;
    error[0] = 0;
  } else {
    strlcpy(error, err.c_str(), sizeof error);
    version++;  // redraw to show the problem
  }
  return true;
}

void begin() {
  loadConfig();
}

void reload() {
  loadConfig();
  fetchNow = true;
}

void pause(bool on) { paused = on; }

// ---- text -----------------------------------------------------------------------------

// WMO weather codes (Open-Meteo), in flash: a switch here became a 400-byte jump table in RAM.
struct Wmo {
  uint8_t code;
  char text[34];
};
static const Wmo WMO[] PROGMEM = {
    {0, "Ясно"},
    {1, "Переважно ясно"},
    {2, "Мінлива хмарність"},
    {3, "Хмарно"},
    {45, "Туман"},
    {48, "Туман з інеєм"},
    {51, "Слабка мряка"},
    {53, "Мряка"},
    {55, "Сильна мряка"},
    {56, "Крижана мряка"},
    {57, "Крижана мряка"},
    {61, "Слабкий дощ"},
    {63, "Дощ"},
    {65, "Сильний дощ"},
    {66, "Крижаний дощ"},
    {67, "Крижаний дощ"},
    {71, "Слабкий сніг"},
    {73, "Сніг"},
    {75, "Сильний сніг"},
    {77, "Снігова крупа"},
    {80, "Слабка злива"},
    {81, "Злива"},
    {82, "Сильна злива"},
    {85, "Снігопад"},
    {86, "Сильний снігопад"},
    {95, "Гроза"},
    {96, "Гроза з градом"},
    {99, "Гроза з градом"},
};

static String describe(int code) {
  for (const Wmo &w : WMO)
    if (pgm_read_byte(&w.code) == code) return FPSTR(w.text);
  return String();
}

static String deg(float t) { return String(lround(t)) + F("°"); }

// ---- icon -----------------------------------------------------------------------------

static void cloud(TFT_eSPI &tft, int x, int y, uint16_t c) {  // centred at x,y, ~70x40
  tft.fillCircle(x - 18, y + 4, 14, c);
  tft.fillCircle(x + 2, y - 8, 20, c);
  tft.fillCircle(x + 22, y + 4, 14, c);
  tft.fillRoundRect(x - 32, y + 4, 68, 16, 8, c);
}

static void sun(TFT_eSPI &tft, int x, int y, int r) {
  for (int a = 0; a < 360; a += 45) {
    float rad = a * DEG_TO_RAD;
    tft.drawWideLine(x + cos(rad) * (r + 5), y + sin(rad) * (r + 5), x + cos(rad) * (r + 13), y + sin(rad) * (r + 13), 4,
                     0xFE60, TFT_BLACK);
  }
  tft.fillCircle(x, y, r, 0xFE60);
}

static void moon(TFT_eSPI &tft, int x, int y, int r) {
  tft.fillCircle(x, y, r, 0xEF5D);
  tft.fillCircle(x + r / 2, y - r / 3, r, TFT_BLACK);
}

// Weather icon in the 96x96 box at (x, y) for a WMO code.
static void drawIcon(TFT_eSPI &tft, int x, int y, int code, bool day) {
  int cx = x + 48, cy = y + 48;
  uint16_t grey = 0xBDF7, dark = 0x7BEF;
  bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
  bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  bool storm = code >= 95;
  if (code == 0) return day ? sun(tft, cx, cy, 22) : moon(tft, cx, cy, 24);
  if (code == 1 || code == 2) {
    if (day) sun(tft, cx - 14, cy - 14, 16);
    else moon(tft, cx - 14, cy - 14, 16);
    return cloud(tft, cx + 6, cy + 12, grey);
  }
  if (code == 45 || code == 48) {
    for (int i = 0; i < 4; i++) tft.fillRoundRect(x + 10 + (i % 2) * 8, y + 26 + i * 14, 70, 6, 3, grey);
    return;
  }
  cloud(tft, cx, cy - 8, storm || rain ? dark : grey);
  if (rain)
    for (int i = 0; i < 4; i++) tft.drawWideLine(cx - 24 + i * 15, cy + 22, cx - 30 + i * 15, cy + 36, 3, 0x4C9F, TFT_BLACK);
  if (snow)
    for (int i = 0; i < 4; i++) tft.fillCircle(cx - 24 + i * 15, cy + 28 + (i % 2) * 8, 4, TFT_WHITE);
  if (storm) {
    tft.fillTriangle(cx + 2, cy + 14, cx - 10, cy + 34, cx + 2, cy + 30, 0xFE60);
    tft.fillTriangle(cx + 2, cy + 30, cx + 12, cy + 30, cx - 4, cy + 46, 0xFE60);
  }
}

// Reading icons, 24 px wide, vertically centred on cy.
static void dropIcon(TFT_eSPI &tft, int x, int cy, uint16_t c) {
  tft.fillTriangle(x + 12, cy - 13, x + 4, cy + 2, x + 20, cy + 2, c);
  tft.fillCircle(x + 12, cy + 4, 8, c);
}

static void windIcon(TFT_eSPI &tft, int x, int cy, uint16_t c) {
  tft.drawWideLine(x, cy - 7, x + 18, cy - 7, 3, c, TFT_BLACK);
  tft.drawWideLine(x, cy, x + 24, cy, 3, c, TFT_BLACK);
  tft.drawWideLine(x + 5, cy + 7, x + 16, cy + 7, 3, c, TFT_BLACK);
}

// ---- screen ---------------------------------------------------------------------------

static uint32_t drawnVersion = UINT32_MAX;

static void render(TFT_eSPI &tft) {
  Data d;
  Config c;
  char err[sizeof error];
  d = data, c = cfg, drawnVersion = version;
  memcpy(err, error, sizeof err);

  tft.fillScreen(TFT_BLACK);
  if (!d.valid) {
    useFont(tft, FONT_LARGE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(*err ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
    tft.drawString(*err ? String(err) : String(F("завантаження…")), 120, 120);
    return;
  }

  // Layout (mocked on a PC first): 12 px margins, two 108 px columns centred on x 66 and 174.
  // Top: icon | temperature + "як <feels>"; middle: description across; thin divider;
  // bottom: humidity | wind, each centred in its column (value 24 px, unit 15 px).
  const int L = 66, R = 174, LIGHT = 0xBDF7, VALUE = 0xEF5D;
  drawIcon(tft, L - 50, 18, d.code, d.day);

  useFont(tft, FONT_TEMP);
  String t = String(lround(d.temp));
  int w = tft.textWidth(t), x = R - (w + 16) / 2;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(t, x, 58);
  tft.drawCircle(x + w + 9, 36, 6, TFT_WHITE);
  tft.drawCircle(x + w + 9, 36, 5, TFT_WHITE);
  useFont(tft, FONT_LARGE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(LIGHT, TFT_BLACK);
  tft.drawString(F("як ") + deg(d.feels), R, 100);

  String desc = *err ? String(err) : String(describe(d.code));
  if (tft.textWidth(desc) > 228) useFont(tft, FONT_SMALL);
  tft.setTextColor(*err ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
  tft.drawString(desc, 120, 140);

  tft.drawFastHLine(24, 166, 192, 0x2945);

  struct Cell {
    int cx;
    String value, unit;
  };
  String windUnit = c.wind.equals(F("km/h")) ? F("км/год") : c.wind.equals(F("mile/h")) ? F("миль/год") : F("м/с");
  Cell cells[2] = {{L, String(d.humidity), "%"}, {R, String(d.wind, d.wind < 10 && c.wind.equals(F("m/s")) ? 1 : 0), windUnit}};
  for (int i = 0; i < 2; i++) {
    useFont(tft, FONT_LARGE);
    int vw = tft.textWidth(cells[i].value);
    useFont(tft, FONT_SMALL);
    int uw = tft.textWidth(cells[i].unit);
    int x0 = cells[i].cx - (24 + 6 + vw + 3 + uw) / 2;
    if (i == 0) dropIcon(tft, x0, 198, 0x5D9F);
    else windIcon(tft, x0, 198, LIGHT);
    tft.setTextDatum(ML_DATUM);
    useFont(tft, FONT_LARGE);
    tft.setTextColor(VALUE, TFT_BLACK);
    tft.drawString(cells[i].value, x0 + 30, 198);
    useFont(tft, FONT_SMALL);
    tft.setTextColor(LIGHT, TFT_BLACK);
    tft.drawString(cells[i].unit, x0 + 30 + vw + 3, 202);
  }
}

void draw(TFT_eSPI &tft) {
  render(tft);
  wifiMarkRedraw(tft);
}

void tick(TFT_eSPI &tft) {
  if (drawnVersion != version) {
    render(tft);
    wifiMarkRedraw(tft);
  }
  wifiMarkTick(tft);
}

String statusJson() {
  Data d;
  Config c;
  char err[sizeof error];
  d = data, c = cfg;
  memcpy(err, error, sizeof err);
  return String(F("{\"city\":\"")) + c.city + F("\",\"place\":\"") + d.place + F("\",\"valid\":") + d.valid +
         F(",\"temp\":") + d.temp + F(",\"feels\":") + d.feels + F(",\"max\":") + d.tmax + F(",\"min\":") + d.tmin +
         F(",\"humidity\":") + d.humidity + F(",\"code\":") + d.code + F(",\"wind\":") + d.wind + F(",\"pressure\":") + d.pressureHpa +
         F(",\"updated\":") + (long)d.updated + F(",\"error\":\"") + err + F("\"}");
}

}  // namespace weather
