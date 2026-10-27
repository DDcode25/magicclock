#include "alerts.h"
#include <algorithm>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "apps.h"
#include "fonts.h"
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
static char error[48] = "";  // last fetch error, guarded by lock
static SemaphoreHandle_t lock;  // guards cfg, snap, version, updatedAt, error (Strings: no spinlock)
static volatile bool fetchNow = true;
static volatile bool paused = false;

static TFT_eSprite *map8 = nullptr;
static uint32_t drawnVersion = UINT32_MAX;

static const int MAP_Y = 2, MAP_H = 160;  // matches BOX_Y/BOX_H in tools/gen_ukraine_map.py

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
    if (c.token.isEmpty()) return err = "немає токена", false;
    url = "https://api.alerts.in.ua/v1/iot/active_air_raid_alerts_by_oblast.json?token=" + c.token;
  } else url = "https://vadimklimenko.com/map/statuses.json";

  WiFiClientSecure client;
  client.setInsecure();  // no CA bundle on the device; data is public and read-only
  HTTPClient http;
  http.setTimeout(10000);
  // Some sources reject unknown clients; present a desktop Chrome.
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36");
  if (!http.begin(client, url)) return err = "немає з'єднання", false;
  int code = http.GET();
  if (code != 200) {
    err = "помилка HTTP " + String(code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  bool ok = c.src == "ubilling"     ? parseUbilling(body.c_str(), body.length(), out)
            : c.src == "neptun"     ? parseNeptun(body.c_str(), body.length(), out)
            : c.src == "alertsinua" ? parseAlertsInUa(body.c_str(), body.length(), out)
                                    : parseKlymenko(body.c_str(), body.length(), out);
  if (!ok) err = "погані дані";
  return ok;
}

static void task(void *) {
  uint32_t last = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (paused || !settings.appEnabled[APP_ALERTS] || !net::connected()) continue;
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

void pause(bool on) { paused = on; }

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
  char levels[REGIONS + 1], raions[RAIONS + 1];
  for (int i = 0; i < REGIONS; i++) levels[i] = "NPAU"[s.level[i]];
  levels[REGIONS] = 0;
  for (int i = 0; i < RAIONS; i++) raions[i] = "NYRI"[s.raion[i]];
  raions[RAIONS] = 0;
  return String("{\"levels\":\"") + levels + "\",\"raions\":\"" + raions + "\",\"updated\":" + (long)upd + ",\"error\":\"" + err + "\",\"src\":\"" +
         c.src + "\",\"home\":" + c.home + "}";
}

// ---- drawing --------------------------------------------------------------------------

static uint16_t levelColor(uint8_t l) {
  switch (l) {
    case FULL: return 0xC8A3;     // red (200,20,24)
    case PARTIAL: return 0xE460;  // orange (230,140,0)
    case UNKNOWN: return 0x52AA;  // grey (80,84,80): no data from this source
    default: return 0x22C5;       // dark green (32,88,40)
  }
}

static uint16_t raionColor(uint8_t l) {
  switch (l) {
    case R_RED: return 0xC8A3;     // same red as a whole-oblast alert
    case R_YELLOW: return 0xE460;  // orange: drone-level alert
    default: return 0x22C5;        // dark green
  }
}

// Even-odd scanline fill of rings [firstRing, firstRing + rings) into the 8-bit map sprite.
// Vertices are in 1/MAP_FIX px; a pixel is filled when its centre lies inside (standard
// rasterisation rule), so shapes keep sub-pixel accuracy and neighbours share edges without gaps.
static void fillRings(int firstRing, int rings, uint16_t color) {
  const int F = MAP_FIX, H = MAP_FIX / 2;
  int y0 = INT16_MAX, y1 = INT16_MIN;
  for (int k = 0; k < rings; k++) {
    const MapRing &ring = MAP_RINGS[firstRing + k];
    for (int i = 0; i < ring.count; i++) {
      int y = MAP_POINTS[ring.first + i][1];
      y0 = min(y0, y), y1 = max(y1, y);
    }
  }
  static int32_t xs[96];
  for (int py = max(y0 / F, MAP_Y); py <= min(y1 / F, MAP_Y + MAP_H - 1); py++) {
    int32_t yc = py * F + H;  // pixel centre
    int n = 0;
    for (int k = 0; k < rings; k++) {
      const MapRing &ring = MAP_RINGS[firstRing + k];
      for (int i = 0; i < ring.count && n < 96; i++) {
        const int16_t *a = MAP_POINTS[ring.first + i], *b = MAP_POINTS[ring.first + (i + 1) % ring.count];
        if ((a[1] <= yc && yc < b[1]) || (b[1] <= yc && yc < a[1]))
          xs[n++] = a[0] + (int32_t)(yc - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
      }
    }
    std::sort(xs, xs + n);
    for (int i = 0; i + 1 < n; i += 2) {
      int xa = (xs[i] - H + F - 1) / F, xb = (xs[i + 1] - H) / F;  // first/last pixel centre inside
      if (xb >= xa) map8->drawFastHLine(xa, py - MAP_Y, xb - xa + 1, color);
    }
  }
}

// Oblast borders only (raion borders are not drawn).
static void outlineRegion(int r, uint16_t color) {
  const MapRegion &reg = MAP[r];
  for (int k = 0; k < reg.rings; k++) {
    const MapRing &ring = MAP_RINGS[reg.firstRing + k];
    for (int i = 0; i < ring.count; i++) {
      const int16_t *a = MAP_POINTS[ring.first + i], *b = MAP_POINTS[ring.first + (i + 1) % ring.count];
      map8->drawLine(a[0] / MAP_FIX, a[1] / MAP_FIX - MAP_Y, b[0] / MAP_FIX, b[1] / MAP_FIX - MAP_Y, color);
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
  // Oblasts first (the only layer for sources without raion data), then raions on top,
  // then Kyiv city and Sevastopol, which sit inside other regions. Only oblast borders are drawn.
  for (int r = 0; r < REGIONS; r++)
    if (r != 9 && r != 18) fillRings(MAP[r].firstRing, MAP[r].rings, levelColor(s.level[r]));
  for (int i = 0; i < MAP_RAIONS; i++)
    if (s.raion[i] != R_INHERIT && s.level[RAION[i].oblast] != UNKNOWN)
      fillRings(RAION[i].firstRing, RAION[i].rings, raionColor(s.raion[i]));
  for (int r : {9, 18}) fillRings(MAP[r].firstRing, MAP[r].rings, levelColor(s.level[r]));
  for (int r = 0; r < REGIONS; r++) outlineRegion(r, TFT_BLACK);
  if (home >= 0) outlineRegion(home, TFT_WHITE);
  map8->pushSprite(0, MAP_Y);
}


static String hhmm(time_t utc) {
  struct tm t;
  localtime_r(&utc, &t);
  char b[8];
  strftime(b, sizeof b, "%H:%M", &t);
  return b;
}

// Under the map, on one line: home city (coloured by its alert level) and the clock, in the
// largest font that fits both (sized per city at build time). A fetch problem, if any, goes small
// in the bottom-left corner.
static void drawFooter(TFT_eSPI &tft, const Snapshot &s, int home, time_t upd, const char *err, int interval) {
  const int y = 199;
  tft.fillRect(0, MAP_Y + MAP_H, 240, 240 - MAP_Y - MAP_H, TFT_BLACK);

  useCityFont(tft, home);
  String clock = net::timeValid() ? hhmm(time(nullptr)) : String("--:--");
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(clock, 237, y);

  if (home >= 0) {
    uint8_t l = s.level[home];
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(!upd || l == UNKNOWN ? TFT_DARKGREY : l == FULL ? TFT_RED : l == PARTIAL ? TFT_ORANGE : TFT_GREEN, TFT_BLACK);
    tft.drawString(MAP[home].label, 3, y);
  }

  bool stale = upd && time(nullptr) - upd > 3 * interval + 30;
  if (*err || stale) {
    useFont(tft, FONT_SMALL);
    String problem = *err ? String(err) : "дані з " + hhmm(upd);
    if (tft.textWidth(problem) > 234) problem = *err ? "помилка" : "старі дані";
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(problem, 3, 231);
  }
}

static void render(TFT_eSPI &tft, bool full) {
  Snapshot s;
  uint32_t v;
  time_t upd;
  char err[sizeof error];
  int home, interval;
  xSemaphoreTake(lock, portMAX_DELAY);
  s = snap, v = version, upd = updatedAt, home = cfg.home, interval = cfg.interval;
  memcpy(err, error, sizeof err);
  xSemaphoreGive(lock);
  if (full || v != drawnVersion) {
    drawnVersion = v;
    drawMap(tft, s, home);
  }
  drawFooter(tft, s, home, upd, err, interval);
}

// ---- home alarm ---------------------------------------------------------------------

static const uint32_t BLINK_MS = 10000, BLINK_PHASE_MS = 500;
static uint32_t blinkStart = 0;
static uint8_t blinkLevel = NONE;
static bool blinking = false;

bool homeAlertStarted() {
  static uint32_t seenVersion = 0;
  static int seenHome = -2;
  static uint8_t prev = UNKNOWN;
  uint32_t v;
  int home;
  uint8_t level;
  xSemaphoreTake(lock, portMAX_DELAY);
  v = version, home = cfg.home;
  level = home >= 0 ? snap.level[home] : NONE;
  xSemaphoreGive(lock);
  if (v == seenVersion && home == seenHome) return false;
  bool first = seenVersion == 0 || home != seenHome;  // boot or home changed: just remember
  seenVersion = v, seenHome = home;
  uint8_t before = prev;
  prev = level;
  if (first || v == 0) return false;
  bool started = (level == FULL || level == PARTIAL) && (before == NONE || before == UNKNOWN || (before == PARTIAL && level == FULL));
  if (started) {
    blinking = true;
    blinkStart = millis();
    blinkLevel = level;
  }
  return started;
}

void testAlarm(uint8_t level) {
  blinkLevel = level == PARTIAL ? PARTIAL : FULL;
  blinkStart = millis();
  blinking = true;
}

static void drawAlarmFrame(TFT_eSPI &tft, int home) {
  // Pure red for a full alert: the map red (200,20,24) looks pink as a full-screen fill.
  uint16_t bg = blinkLevel == FULL ? 0xF800 : levelColor(blinkLevel);
  tft.fillScreen(bg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_BLACK, bg);
  useFont(tft, FONT_LARGE);
  tft.drawString("ТРИВОГА", 120, 105);  // full vs partial differs by colour only
  if (home >= 0) tft.drawString(MAP[home].label, 120, 140);
}

void draw(TFT_eSPI &tft) {
  tft.fillScreen(TFT_BLACK);
  render(tft, true);
}

void loop(TFT_eSPI &tft) {
  if (!blinking) return;
  uint32_t t = millis() - blinkStart;
  static int shownPhase = -1;
  int phase = t / BLINK_PHASE_MS;
  if (t >= BLINK_MS) {
    blinking = false;
    shownPhase = -1;
    draw(tft);
    return;
  }
  if (phase == shownPhase) return;
  shownPhase = phase;
  if (phase % 2 == 0) {
    int home;
    xSemaphoreTake(lock, portMAX_DELAY);
    home = cfg.home;
    xSemaphoreGive(lock);
    drawAlarmFrame(tft, home);
  } else {
    draw(tft);
  }
}

void tick(TFT_eSPI &tft) {
  if (blinking) return;  // loop() owns the screen while the alarm blinks
  static uint32_t lastFooter = 0;
  if (drawnVersion != version || millis() - lastFooter > 5000) {
    lastFooter = millis();
    render(tft, false);
  }
}

}  // namespace alerts
