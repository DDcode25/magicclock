#include "alerts.h"
#include <algorithm>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "apps.h"
#include "net.h"
#include "store.h"
#include "ukraine_map.h"

namespace alerts {

static const char *CONFIG = "/.sys/alerts.json";
static const int MIN_INTERVAL_S = 15;  // alerts.in.ua allows ~10 requests/min per IP

struct Config {
  String src = "klymenko";
  String token;
  int interval = 30;
  int home = -1;
};

static Config cfg;
static Snapshot snap;
static uint32_t version = 0;   // bumped on each successful fetch
static time_t updatedAt = 0;   // UTC of last successful fetch
static char error[24] = "";  // last fetch error, guarded by lock
static SemaphoreHandle_t lock;  // guards cfg, snap, version, updatedAt, error (Strings: no spinlock)
static volatile bool fetchNow = true;

static TFT_eSprite *map8 = nullptr;
static uint32_t drawnVersion = UINT32_MAX;

static const int MAP_Y = 40, MAP_H = 160;

static void loadConfig() {
  Config c;
  c.src = store::getStr(CONFIG, "src", "klymenko");
  c.token = store::getStr(CONFIG, "token", "");
  c.interval = max(MIN_INTERVAL_S, store::getInt(CONFIG, "i", 30));
  c.home = constrain(store::getInt(CONFIG, "home", -1), -1, REGIONS - 1);
  xSemaphoreTake(lock, portMAX_DELAY);
  cfg = c;
  xSemaphoreGive(lock);
}

static bool fetch(const Config &c, Snapshot &out, String &err) {
  String url;
  if (c.src == "ubilling") url = "https://ubilling.net.ua/aerialalerts/";
  else if (c.src == "neptun") url = "https://neptun.in.ua/api/v1/alerts";
  else if (c.src == "alertsinua") {
    if (c.token.isEmpty()) return err = "no token", false;
    url = "https://api.alerts.in.ua/v1/iot/active_air_raid_alerts_by_oblast.json?token=" + c.token;
  } else url = "https://vadimklimenko.com/map/statuses.json";

  WiFiClientSecure client;
  client.setInsecure();  // no CA bundle on the device; data is public and read-only
  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent("SmallTV-PRO-OpenFW");
  if (!http.begin(client, url)) return err = "connect", false;
  int code = http.GET();
  if (code != 200) {
    err = "HTTP " + String(code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  bool ok = c.src == "ubilling"     ? parseUbilling(body.c_str(), body.length(), out)
            : c.src == "neptun"     ? parseNeptun(body.c_str(), body.length(), out)
            : c.src == "alertsinua" ? parseAlertsInUa(body.c_str(), body.length(), out)
                                    : parseKlymenko(body.c_str(), body.length(), out);
  if (!ok) err = "bad data";
  return ok;
}

static void task(void *) {
  uint32_t last = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!settings.appEnabled[APP_ALERTS] || !net::connected()) continue;
    Config c;
    xSemaphoreTake(lock, portMAX_DELAY);
    c = cfg;
    xSemaphoreGive(lock);
    if (!fetchNow && millis() - last < (uint32_t)c.interval * 1000) continue;
    fetchNow = false;
    last = millis();

    Snapshot s;
    String err;
    bool ok = fetch(c, s, err);
    Serial.printf("alerts: %s %s\n", c.src.c_str(), ok ? "ok" : err.c_str());
    xSemaphoreTake(lock, portMAX_DELAY);
    if (ok) {
      snap = s;
      updatedAt = time(nullptr);
      version++;
      error[0] = 0;
    } else {
      strlcpy(error, err.c_str(), sizeof error);
    }
    xSemaphoreGive(lock);
  }
}

void begin() {
  lock = xSemaphoreCreateMutex();
  loadConfig();
  memset(&snap, 0, sizeof snap);
  xTaskCreatePinnedToCore(task, "alerts", 12288, nullptr, 1, nullptr, 0);
}

void reload() {
  loadConfig();
  fetchNow = true;
  drawnVersion = UINT32_MAX;
}

String statusJson() {
  Snapshot s;
  time_t upd;
  char err[sizeof error];
  Config c;
  xSemaphoreTake(lock, portMAX_DELAY);
  s = snap, upd = updatedAt;
  memcpy(err, error, sizeof err);
  c = cfg;
  xSemaphoreGive(lock);
  char levels[REGIONS + 1];
  for (int i = 0; i < REGIONS; i++) levels[i] = "NPA"[s.level[i]];
  levels[REGIONS] = 0;
  return String("{\"levels\":\"") + levels + "\",\"updated\":" + (long)upd + ",\"error\":\"" + err + "\",\"src\":\"" +
         c.src + "\",\"home\":" + c.home + "}";
}

// ---- drawing --------------------------------------------------------------------------

static uint16_t levelColor(uint8_t l) {
  switch (l) {
    case FULL: return 0xC8A3;     // red (200,20,24)
    case PARTIAL: return 0xE460;  // orange (230,140,0)
    default: return 0x22C5;       // dark green (32,88,40)
  }
}

// Even-odd scanline fill of all rings of a region, into the 8-bit map sprite.
static void fillRegion(int r, uint16_t color) {
  const MapRegion &reg = MAP[r];
  int y0 = 255, y1 = 0;
  for (int k = 0; k < reg.rings; k++) {
    const MapRing &ring = MAP_RINGS[reg.firstRing + k];
    for (int i = 0; i < ring.count; i++) {
      int y = MAP_POINTS[ring.first + i][1];
      y0 = min(y0, y), y1 = max(y1, y);
    }
  }
  static int16_t xs[64];
  for (int y = y0; y <= y1; y++) {
    int n = 0;
    for (int k = 0; k < reg.rings; k++) {
      const MapRing &ring = MAP_RINGS[reg.firstRing + k];
      for (int i = 0; i < ring.count && n < 64; i++) {
        const uint8_t *a = MAP_POINTS[ring.first + i], *b = MAP_POINTS[ring.first + (i + 1) % ring.count];
        if ((a[1] <= y && y < b[1]) || (b[1] <= y && y < a[1]))
          xs[n++] = a[0] + (int32_t)(y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
      }
    }
    std::sort(xs, xs + n);
    for (int i = 0; i + 1 < n; i += 2) map8->drawFastHLine(xs[i], y - MAP_Y, xs[i + 1] - xs[i] + 1, color);
  }
}

static void outlineRegion(int r, uint16_t color) {
  const MapRegion &reg = MAP[r];
  for (int k = 0; k < reg.rings; k++) {
    const MapRing &ring = MAP_RINGS[reg.firstRing + k];
    for (int i = 0; i < ring.count; i++) {
      const uint8_t *a = MAP_POINTS[ring.first + i], *b = MAP_POINTS[ring.first + (i + 1) % ring.count];
      map8->drawLine(a[0], a[1] - MAP_Y, b[0], b[1] - MAP_Y, color);
    }
  }
}

static void drawMap(TFT_eSPI &tft, const Snapshot &s, int home) {
  if (!map8) {
    map8 = new TFT_eSprite(&tft);
    map8->setColorDepth(8);
    if (!map8->createSprite(240, MAP_H)) {
      delete map8;
      map8 = nullptr;
      return;
    }
  }
  map8->fillSprite(TFT_BLACK);
  // Kyiv city and Sevastopol sit inside other regions: draw them last.
  for (int pass = 0; pass < 2; pass++)
    for (int r = 0; r < REGIONS; r++)
      if ((r == 9 || r == 18) == (pass == 1)) fillRegion(r, levelColor(s.level[r]));
  for (int r = 0; r < REGIONS; r++) outlineRegion(r, TFT_BLACK);
  if (home >= 0) outlineRegion(home, TFT_WHITE);
  map8->pushSprite(0, MAP_Y);
}

static void drawHeader(TFT_eSPI &tft, const Snapshot &s) {
  int count = 0;
  for (int r = 0; r < REGIONS; r++) count += s.level[r] != NONE;
  tft.fillRect(0, 0, 240, 36, 0x18E3);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_ORANGE, 0x18E3);
  tft.drawString("AIR ALERTS", 8, 18, 4);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(count ? TFT_RED : TFT_GREEN, 0x18E3);
  tft.drawString(String(count) + "/" + REGIONS, 232, 18, 4);
}

static String hhmm(time_t utc) {
  struct tm t;
  localtime_r(&utc, &t);
  char b[8];
  strftime(b, sizeof b, "%H:%M", &t);
  return b;
}

// Data credit shown under the map (NEPTUN terms require a visible link next to the data).
static const char *sourceDomain(const String &src) {
  if (src == "neptun") return "neptun.in.ua";
  if (src == "ubilling") return "ubilling.net.ua";
  if (src == "alertsinua") return "alerts.in.ua";
  return "vadimklimenko.com";
}

static void drawFooter(TFT_eSPI &tft, const Snapshot &s, int home, const String &src, time_t upd, const char *err) {
  tft.fillRect(0, 202, 240, 38, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  if (home >= 0) {
    uint8_t l = s.level[home];
    String state = l == FULL ? "ALERT" : l == PARTIAL ? "partial" : "clear";
    if (l != NONE && s.since[home]) state += " since " + hhmm(s.since[home]);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(MAP[home].label, 6, 212, 2);
    tft.setTextColor(l == FULL ? TFT_RED : l == PARTIAL ? TFT_ORANGE : TFT_GREEN, TFT_BLACK);
    tft.drawString(state, 6, 230, 2);
  }
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(sourceDomain(src), 234, 212, 2);
  String status;
  if (*err) status = err;
  else if (!upd) status = "loading...";
  else status = "upd " + hhmm(upd);
  tft.setTextColor(*err ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
  tft.drawString(status, 234, 230, 2);
}

static void render(TFT_eSPI &tft, bool full) {
  Snapshot s;
  uint32_t v;
  time_t upd;
  char err[sizeof error];
  int home;
  String src;
  xSemaphoreTake(lock, portMAX_DELAY);
  s = snap, v = version, upd = updatedAt, home = cfg.home, src = cfg.src;
  memcpy(err, error, sizeof err);
  xSemaphoreGive(lock);
  if (full || v != drawnVersion) {
    drawnVersion = v;
    drawHeader(tft, s);
    drawMap(tft, s, home);
  }
  drawFooter(tft, s, home, src, upd, err);
}

void draw(TFT_eSPI &tft) {
  tft.fillScreen(TFT_BLACK);
  render(tft, true);
}

void tick(TFT_eSPI &tft) {
  static uint32_t lastFooter = 0;
  if (drawnVersion != version || millis() - lastFooter > 5000) {
    lastFooter = millis();
    render(tft, false);
  }
}

}  // namespace alerts
