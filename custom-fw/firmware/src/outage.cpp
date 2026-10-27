#include "outage.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <time.h>
#include "apps.h"
#include "fonts.h"
#include "net.h"
#include "netreader.h"
#include "outage_parse.h"
#include "store.h"
#include "widgets.h"

namespace outagescreen {

using outage::Schedule;

static const char *CONFIG = "/.sys/outage.json";

struct Config {
  String src = "y:Poltavaoblenergo";
  String group = "1.1";
  int intervalMin = 10;
};

static SemaphoreHandle_t lock;
static Config cfg;
static Schedule sched;
static bool valid = false;
static uint32_t version = 0;
static time_t updatedAt = 0;
static char error[48] = "";
static volatile bool fetchNow = true, paused = false;

static void loadConfig() {
  Config c;
  c.src = store::getStr(CONFIG, "src", "y:Poltavaoblenergo");
  c.group = store::getStr(CONFIG, "grp", "1.1");
  c.intervalMin = max(5, store::getInt(CONFIG, "i", 10));
  xSemaphoreTake(lock, portMAX_DELAY);
  bool changed = c.src != cfg.src || c.group != cfg.group;
  cfg = c;
  if (changed) valid = false;
  xSemaphoreGive(lock);
}

static String sourceUrl(const String &src) {
  String name = src.substring(2);
  if (src.startsWith("b:")) return "https://raw.githubusercontent.com/Baskerville42/outage-data-ua/main/data/" + name + ".json";
  return "https://raw.githubusercontent.com/yaroslav2901/OE_OUTAGE_DATA/main/data/" + name + ".json";
}

static bool fetch(const Config &c, Schedule &s, String &err) {
  WiFiClientSecure client;
  client.setInsecure();  // public, read-only data; no CA bundle on the device
  HTTPClient http;
  http.setTimeout(15000);
  http.useHTTP10(true);  // plain body: parsed straight from the stream (files up to 300 KB)
  if (!http.begin(client, sourceUrl(c.src))) return err = "немає з’єднання", false;
  int code = http.GET();
  if (code != 200) {
    err = "помилка HTTP " + String(code);
    http.end();
    return false;
  }
  NetReader reader{http.getStream(), 15000};
  bool ok = outage::parse(reader, c.group.c_str(), s);
  http.end();
  if (!ok) err = String("немає графіка: ") + outage::lastError;
  return ok;
}

static void task(void *) {
  uint32_t last = 0;
  bool lastOk = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (paused || !settings.appEnabled[APP_OUTAGE] || !net::connected()) continue;
    Config c;
    xSemaphoreTake(lock, portMAX_DELAY);
    c = cfg;
    xSemaphoreGive(lock);
    uint32_t period = lastOk ? c.intervalMin * 60000UL : 60000UL;
    if (!fetchNow && last && millis() - last < period) continue;
    fetchNow = false;
    last = millis();

    Schedule s;
    String err;
    net::fetchLock();
    bool ok = fetch(c, s, err);
    net::fetchUnlock();
    lastOk = ok;
    Serial.printf("outage: %s\n", ok ? "ok" : err.c_str());
    xSemaphoreTake(lock, portMAX_DELAY);
    if (ok) {
      sched = s;
      valid = true;
      updatedAt = time(nullptr);
      error[0] = 0;
    } else {
      strlcpy(error, err.c_str(), sizeof error);
    }
    version++;
    xSemaphoreGive(lock);
  }
}

void begin() {
  lock = xSemaphoreCreateMutex();
  loadConfig();
  xTaskCreatePinnedToCore(task, "outage", 10240, nullptr, 1, nullptr, 0);
}

void reload() {
  loadConfig();
  fetchNow = true;
}

void pause(bool on) { paused = on; }

// ---- dial -----------------------------------------------------------------------------
// A spiral ("snail") like the oblenergo cards: one band of width W, two turns a day from the top
// clockwise; its inner radius grows STEP px per half hour, so 12:00 sits one band further out than
// 0:00 and 24:00 one more (the step at the top). Mocked on a PC first.

static const int CX = 120, CY = 120;
static const int R0 = 46, W = 21, GAP = 3;  // inner radius at 0:00, band width, gap between turns
static const float STEP = (W + GAP) / 24.0f;
static const uint16_t C_ON = 0x75E7, C_OFF = 0xE3D1, C_MAYBE = 0xE667, C_NONE = 0x2945, C_INK = 0x10A2;
static int showDay = 0;  // 0 today, 1 tomorrow
static uint32_t drawnVersion = UINT32_MAX;
static int drawnMinute = -1;

static uint16_t slotColor(uint8_t s) {
  return s == outage::ON ? C_ON : s == outage::OFF ? C_OFF : s == outage::MAYBE ? C_MAYBE : C_NONE;
}

static float bandIn(float slot) { return R0 + slot * STEP; }  // slot: half hours since 0:00

// Angle a in degrees from 12 o'clock, clockwise -> point at radius r.
static int px(float a, float r) { return CX + lroundf(r * sinf(a * DEG_TO_RAD)); }
static int py(float a, float r) { return CY - lroundf(r * cosf(a * DEG_TO_RAD)); }

static void drawRings(TFT_eSPI &tft, const uint8_t *slots) {
  for (int i = 0; i < outage::SLOTS; i++) {
    int r0 = lroundf(bandIn(i));
    uint32_t start = ((i % 24) * 15 + 180) % 360;  // TFT_eSPI arcs start at 6 o'clock
    tft.drawArc(CX, CY, r0 + W, r0, start, start + 15, slotColor(slots[i]), TFT_BLACK, true);
  }
  useFont(tft, FONT_DIAL);
  tft.setTextDatum(MC_DATUM);
  for (int h = 0; h < 24; h++) {
    float a = (h % 12) * 30, r0 = bandIn(2 * h);
    tft.drawWideLine(px(a, r0), py(a, r0), px(a, r0 + 5), py(a, r0 + 5), 1.5, C_INK, slotColor(slots[2 * h]));
    tft.drawLine(px(a + 15, r0 + STEP), py(a + 15, r0 + STEP), px(a + 15, r0 + STEP + 3), py(a + 15, r0 + STEP + 3), C_INK);
    // Hour label: black, drawn twice 1 px apart for extra weight (the band is pale and the font small).
    int lx = px(a + 8, r0 + W * 0.55f), ly = py(a + 8, r0 + W * 0.55f);
    tft.setTextColor(TFT_BLACK, slotColor(slots[2 * h]));
    tft.drawString(String(h), lx, ly);
    tft.drawString(String(h), lx + 1, ly);
  }
}

// The marker sticks out of the band, so the old one is erased before the dial is repainted.
static float markA = -1;
static int markR0, markR1;

static void eraseNow(TFT_eSPI &tft) {
  if (markA < 0) return;
  tft.drawWideLine(px(markA, markR0), py(markA, markR0), px(markA, markR1), py(markA, markR1), 5, TFT_BLACK, TFT_BLACK);
  markA = -1;
}

static void drawNow(TFT_eSPI &tft) {
  if (showDay != 0 || !net::timeValid()) return;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  float slot = (t.tm_hour * 60 + t.tm_min) / 30.0f, r0 = bandIn(slot);
  markA = fmodf(slot * 15, 360);
  markR0 = lroundf(r0) - 2, markR1 = lroundf(r0) + W + 2;
  tft.drawWideLine(px(markA, markR0), py(markA, markR0), px(markA, markR1), py(markA, markR1), 3, TFT_WHITE, TFT_BLACK);
}

static String hoursText(int slots) { return slots % 2 ? String(slots / 2) + ".5" : String(slots / 2); }

// Centre, one under another: queue, hours without power (possible outages counted in, as on the
// oblenergo cards), hours with power. "+18.5" in 20 px just fits the circle on its line.
// "завтра" goes to the free top-left corner when showing tomorrow; a fetch error replaces the hours.
static void drawCenter(TFT_eSPI &tft, const Config &c, const uint8_t *slots, const char *err) {
  tft.fillCircle(CX, CY, R0 - 3, TFT_BLACK);
  tft.fillRect(0, 0, 70, 22, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  if (showDay) {
    useFont(tft, FONT_SMALL);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("завтра", 4, 4);
  }
  tft.setTextDatum(MC_DATUM);
  useFont(tft, FONT_LARGE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(c.group, CX, 100);
  if (*err) {
    useFont(tft, FONT_SMALL);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(String(err).substring(0, 12), CX, 128);
    return;
  }
  int on = 0, off = 0;
  for (int i = 0; i < outage::SLOTS; i++) on += slots[i] == outage::ON, off += slots[i] == outage::OFF || slots[i] == outage::MAYBE;
  useFont(tft, FONT_NUM);
  tft.setTextColor(C_OFF, TFT_BLACK);
  tft.drawString("-" + hoursText(off), CX, 120);
  tft.setTextColor(C_ON, TFT_BLACK);
  tft.drawString("+" + hoursText(on), CX, 138);
}

static void render(TFT_eSPI &tft, bool clear) {
  Config c;
  Schedule s;
  bool ok;
  char err[sizeof error];
  xSemaphoreTake(lock, portMAX_DELAY);
  c = cfg, s = sched, ok = valid, drawnVersion = version;
  memcpy(err, error, sizeof err);
  xSemaphoreGive(lock);
  if (clear) tft.fillScreen(TFT_BLACK);
  if (!ok) {
    tft.fillScreen(TFT_BLACK);
    useFont(tft, FONT_LARGE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(*err ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
    tft.drawString(*err ? err : "завантаження…", CX, CY);
    wifiMarkRedraw(tft);
    return;
  }
  if (showDay && !s.hasTomorrow) showDay = 0;
  if (!clear) eraseNow(tft);
  else markA = -1;
  drawRings(tft, s.day[showDay]);
  drawNow(tft);
  drawCenter(tft, c, s.day[showDay], err);
  wifiMarkRedraw(tft);
}

void draw(TFT_eSPI &tft) {
  render(tft, true);
  drawnMinute = -1;
}

void tick(TFT_eSPI &tft) {
  bool redraw = drawnVersion != version;
  if (net::timeValid()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_min != drawnMinute) {
      drawnMinute = t.tm_min;
      redraw = true;  // move the time marker
    }
  }
  if (redraw) render(tft, false);  // rings repaint over themselves: no blank flash
  else wifiMarkTick(tft);
}

void toggleDay(TFT_eSPI &tft) {
  bool tomorrow;
  xSemaphoreTake(lock, portMAX_DELAY);
  tomorrow = valid && sched.hasTomorrow;
  xSemaphoreGive(lock);
  showDay = tomorrow ? !showDay : 0;
  render(tft, true);
}

String statusJson() {
  Config c;
  Schedule s;
  bool ok;
  char err[sizeof error];
  xSemaphoreTake(lock, portMAX_DELAY);
  c = cfg, s = sched, ok = valid;
  memcpy(err, error, sizeof err);
  xSemaphoreGive(lock);
  String days[2];
  for (int d = 0; d < 2; d++)
    for (int i = 0; i < outage::SLOTS; i++) days[d] += "+-?."[s.day[d][i]];
  return String("{\"src\":\"") + c.src + "\",\"group\":\"" + c.group + "\",\"valid\":" + ok + ",\"today\":" +
         (long)s.today + ",\"update\":\"" + s.update + "\",\"day0\":\"" + days[0] + "\",\"day1\":\"" +
         (s.hasTomorrow ? days[1] : String("")) + "\",\"fetched\":" + (long)updatedAt + ",\"error\":\"" + err + "\"}";
}

}  // namespace outagescreen
