#include "outage.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <time.h>
#include "apps.h"
#include "fonts.h"
#include "net.h"
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

// Blocking reader for ArduinoJson: the TLS stream returns "nothing yet" between records, which
// ArduinoJson took for the end of input (EmptyInput). Wait for data while the link is open.
struct NetReader {
  WiFiClient &c;
  uint32_t timeoutMs;
  int read() {
    uint32_t t0 = millis();
    while (!c.available()) {
      if (!c.connected() || millis() - t0 > timeoutMs) return -1;
      delay(2);
    }
    return c.read();
  }
  size_t readBytes(char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
      int ch = read();
      if (ch < 0) break;
      buf[got++] = (char)ch;
      int more = min((int)(n - got), c.available());
      if (more > 0) got += c.read((uint8_t *)buf + got, more);
    }
    return got;
  }
};

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

static const int CX = 120, CY = 120;
static const int IN0 = 54, IN1 = 82, OUT0 = 86, OUT1 = 114;  // ring radii
static const uint16_t C_ON = 0x75E7, C_OFF = 0xE3D1, C_MAYBE = 0xE667, C_NONE = 0x2945;
static int showDay = 0;  // 0 today, 1 tomorrow
static uint32_t drawnVersion = UINT32_MAX;
static int drawnMinute = -1;

static uint16_t slotColor(uint8_t s) {
  return s == outage::ON ? C_ON : s == outage::OFF ? C_OFF : s == outage::MAYBE ? C_MAYBE : C_NONE;
}

// Angle a in degrees from 12 o'clock, clockwise -> point at radius r.
static int px(float a, float r) { return CX + lroundf(r * sinf(a * DEG_TO_RAD)); }
static int py(float a, float r) { return CY - lroundf(r * cosf(a * DEG_TO_RAD)); }

static void drawRings(TFT_eSPI &tft, const uint8_t *slots) {
  for (int i = 0; i < outage::SLOTS; i++) {
    int r0 = i < 24 ? IN0 : OUT0, r1 = i < 24 ? IN1 : OUT1;
    uint32_t start = ((i % 24) * 15 + 180) % 360;  // TFT_eSPI arcs start at 6 o'clock
    tft.drawArc(CX, CY, r1, r0, start, start + 15, slotColor(slots[i]), TFT_BLACK, true);
  }
  for (int h = 0; h < 12; h++) {  // hour separators
    float a = h * 30;
    tft.drawWideLine(px(a, IN0), py(a, IN0), px(a, IN1), py(a, IN1), 2, TFT_BLACK, TFT_BLACK);
    tft.drawWideLine(px(a, OUT0), py(a, OUT0), px(a, OUT1), py(a, OUT1), 2, TFT_BLACK, TFT_BLACK);
  }
  useFont(tft, FONT_SMALL);
  tft.setTextDatum(MC_DATUM);
  for (int h = 0; h < 24; h += 3) {  // hour labels on the rings
    float r = h < 12 ? (IN0 + IN1) / 2.0f : (OUT0 + OUT1) / 2.0f, a = (h % 12) * 30 + 15;
    uint8_t s = slots[h * 2];
    tft.setTextColor(0x18E3, slotColor(s));
    tft.drawString(String(h), px(a, r), py(a, r) + 1);
  }
}

// The marker sticks out of its ring, so the old one is erased before the rings are repainted.
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
  float hours = t.tm_hour + t.tm_min / 60.0f;
  markA = fmodf(hours, 12) * 30;
  markR0 = t.tm_hour < 12 ? IN0 - 3 : OUT0 - 2, markR1 = t.tm_hour < 12 ? IN1 + 2 : OUT1 + 3;
  tft.drawWideLine(px(markA, markR0), py(markA, markR0), px(markA, markR1), py(markA, markR1), 3, TFT_WHITE, TFT_BLACK);
}

static String hoursText(int slots) { return slots % 2 ? String(slots / 2) + ".5" : String(slots / 2); }

static void drawCenter(TFT_eSPI &tft, const Config &c, const Schedule &s, const uint8_t *slots, const char *err) {
  tft.fillCircle(CX, CY, IN0 - 4, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  useFont(tft, FONT_LARGE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(c.group, CX, 96);
  useFont(tft, FONT_SMALL);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("черга", CX, 116);
  static const char *const WD[] = {"нд", "пн", "вт", "ср", "чт", "пт", "сб"};
  time_t day = s.today + showDay * 86400 + 12 * 3600;  // midday: safe against DST shifts
  struct tm t;
  localtime_r(&day, &t);
  char date[24];
  snprintf(date, sizeof date, "%s %02d.%02d", WD[t.tm_wday], t.tm_mday, t.tm_mon + 1);
  tft.drawString(*err ? String(err).substring(0, 14) : String(showDay ? "завтра " : "") + date, CX, 134);

  // Hours without power (possible outages counted in, as on the oblenergo cards) and with power.
  int on = 0, off = 0;
  for (int i = 0; i < outage::SLOTS; i++) on += slots[i] == outage::ON, off += slots[i] == outage::OFF || slots[i] == outage::MAYBE;
  String a = "-" + hoursText(off), b = " +" + hoursText(on);
  int x = CX - tft.textWidth(a + b) / 2;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_OFF, TFT_BLACK);
  tft.drawString(a, x, 152);
  tft.setTextColor(C_ON, TFT_BLACK);
  tft.drawString(b, x + tft.textWidth(a), 152);
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
  drawCenter(tft, c, s, s.day[showDay], err);
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
