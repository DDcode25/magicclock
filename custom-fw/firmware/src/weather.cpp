#include "weather.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <time.h>
#include "apps.h"
#include "fonts.h"
#include "net.h"
#include "store.h"

namespace weather {

static const char *CITY = "/.sys/city.json";
static const char *UNIT = "/.sys/unit.json";

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

static SemaphoreHandle_t lock;
static Config cfg;
static Data data;
static uint32_t version = 0;
static char error[48] = "";
static volatile bool fetchNow = true;

// Geocoder cache: only re-resolve when the city setting changes.
static String geoCity, geoName;
static float lat = 0, lon = 0;

static void loadConfig() {
  Config c;
  c.city = store::getStr(CITY, "cd1", "");
  c.nickname = store::getStr(CITY, "nickname", "");
  c.intervalMin = max(5, store::getInt(CITY, "w_i", 20));
  c.fahrenheit = store::getStr(UNIT, "t_u", "°C") == "°F";
  c.wind = store::getStr(UNIT, "w_u", "m/s");
  c.pressure = store::getStr(UNIT, "p_u", "hPa");
  xSemaphoreTake(lock, portMAX_DELAY);
  cfg = c;
  xSemaphoreGive(lock);
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
  if (!http.begin(client, url)) return err = "немає з’єднання", false;
  int code = http.GET();
  if (code != 200) {
    err = "помилка HTTP " + String(code);
    http.end();
    return false;
  }
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e) return err = "погані дані", false;
  return true;
}

static bool geocode(const Config &c, String &err) {
  if (c.city == geoCity && geoName.length()) return true;
  if (c.city.isEmpty()) return err = "вкажіть місто", false;
  bool numeric = true;
  for (size_t i = 0; i < c.city.length(); i++) numeric &= isDigit(c.city[i]);
  if (numeric) return err = "потрібна назва міста", false;  // OpenWeatherMap city ids need an OWM key
  JsonDocument doc;
  if (!getJson("http://geocoding-api.open-meteo.com/v1/search?count=1&language=uk&format=json&name=" + urlEncode(c.city),
               doc, err))
    return false;
  JsonObject r = doc["results"][0];
  if (r.isNull()) return err = "місто не знайдено", false;
  lat = r["latitude"], lon = r["longitude"];
  geoName = r["name"] | c.city.c_str();
  geoCity = c.city;
  return true;
}

static bool fetch(const Config &c, Data &d, String &err) {
  if (!geocode(c, err)) return false;
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + String(lat, 4) + "&longitude=" + String(lon, 4) +
               "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,"
               "pressure_msl,is_day&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto"
               "&wind_speed_unit=" +
               (c.wind == "km/h" ? "kmh" : c.wind == "mile/h" ? "mph" : "ms") +
               (c.fahrenheit ? "&temperature_unit=fahrenheit" : "");
  JsonDocument doc;
  if (!getJson(url, doc, err)) return false;
  JsonObject cur = doc["current"];
  if (cur.isNull()) return err = "погані дані", false;
  d.place = geoName;
  d.temp = cur["temperature_2m"];
  d.feels = cur["apparent_temperature"];
  d.humidity = cur["relative_humidity_2m"];
  d.code = cur["weather_code"];
  d.wind = cur["wind_speed_10m"];
  d.pressureHpa = cur["pressure_msl"];
  d.day = (cur["is_day"] | 1) == 1;
  d.tmax = doc["daily"]["temperature_2m_max"][0];
  d.tmin = doc["daily"]["temperature_2m_min"][0];
  d.updated = time(nullptr);
  d.valid = true;
  return true;
}

static void task(void *) {
  uint32_t last = 0;
  bool lastOk = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!settings.appEnabled[APP_WEATHER] || !net::connected()) continue;
    Config c;
    xSemaphoreTake(lock, portMAX_DELAY);
    c = cfg;
    xSemaphoreGive(lock);
    uint32_t period = lastOk ? c.intervalMin * 60000UL : 60000UL;  // retry errors every minute
    if (!fetchNow && last && millis() - last < period) continue;
    fetchNow = false;
    last = millis();

    Data d;
    String err;
    net::fetchLock();
    bool ok = fetch(c, d, err);
    net::fetchUnlock();
    lastOk = ok;
    Serial.printf("weather: %s\n", ok ? "ok" : err.c_str());
    xSemaphoreTake(lock, portMAX_DELAY);
    if (ok) {
      data = d;
      version++;
      error[0] = 0;
    } else {
      strlcpy(error, err.c_str(), sizeof error);
      version++;  // redraw to show the problem
    }
    xSemaphoreGive(lock);
  }
}

void begin() {
  lock = xSemaphoreCreateMutex();
  loadConfig();
  xTaskCreatePinnedToCore(task, "weather", 8192, nullptr, 1, nullptr, 0);
}

void reload() {
  loadConfig();
  fetchNow = true;
}

// ---- text -----------------------------------------------------------------------------

static const char *describe(int code) {
  switch (code) {
    case 0: return "Ясно";
    case 1: return "Переважно ясно";
    case 2: return "Мінлива хмарність";
    case 3: return "Хмарно";
    case 45: return "Туман";
    case 48: return "Туман з інеєм";
    case 51: return "Слабка мряка";
    case 53: return "Мряка";
    case 55: return "Сильна мряка";
    case 56: case 57: return "Крижана мряка";
    case 61: return "Слабкий дощ";
    case 63: return "Дощ";
    case 65: return "Сильний дощ";
    case 66: case 67: return "Крижаний дощ";
    case 71: return "Слабкий сніг";
    case 73: return "Сніг";
    case 75: return "Сильний сніг";
    case 77: return "Снігова крупа";
    case 80: return "Слабка злива";
    case 81: return "Злива";
    case 82: return "Сильна злива";
    case 85: return "Снігопад";
    case 86: return "Сильний снігопад";
    case 95: return "Гроза";
    case 96: case 99: return "Гроза з градом";
    default: return "";
  }
}

static String deg(float t) { return String(lround(t)) + "°"; }

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
  xSemaphoreTake(lock, portMAX_DELAY);
  d = data, c = cfg, drawnVersion = version;
  memcpy(err, error, sizeof err);
  xSemaphoreGive(lock);

  tft.fillScreen(TFT_BLACK);
  if (!d.valid) {
    useFont(tft, FONT_LARGE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(*err ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
    tft.drawString(*err ? err : "завантаження…", 120, 120);
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
  tft.drawString("як " + deg(d.feels), R, 100);

  String desc = *err ? String(err) : String(describe(d.code));
  if (tft.textWidth(desc) > 228) useFont(tft, FONT_SMALL);
  tft.setTextColor(*err ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
  tft.drawString(desc, 120, 140);

  tft.drawFastHLine(24, 166, 192, 0x2945);

  struct Cell {
    int cx;
    String value, unit;
  };
  String windUnit = c.wind == "km/h" ? "км/год" : c.wind == "mile/h" ? "миль/год" : "м/с";
  Cell cells[2] = {{L, String(d.humidity), "%"}, {R, String(d.wind, d.wind < 10 && c.wind == "m/s" ? 1 : 0), windUnit}};
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

void draw(TFT_eSPI &tft) { render(tft); }

void tick(TFT_eSPI &tft) {
  if (drawnVersion != version) render(tft);
}

String statusJson() {
  Data d;
  Config c;
  char err[sizeof error];
  xSemaphoreTake(lock, portMAX_DELAY);
  d = data, c = cfg;
  memcpy(err, error, sizeof err);
  xSemaphoreGive(lock);
  return String("{\"city\":\"") + c.city + "\",\"place\":\"" + d.place + "\",\"valid\":" + d.valid +
         ",\"temp\":" + d.temp + ",\"feels\":" + d.feels + ",\"max\":" + d.tmax + ",\"min\":" + d.tmin +
         ",\"humidity\":" + d.humidity + ",\"code\":" + d.code + ",\"wind\":" + d.wind + ",\"pressure\":" + d.pressureHpa +
         ",\"updated\":" + (long)d.updated + ",\"error\":\"" + err + "\"}";
}

}  // namespace weather
