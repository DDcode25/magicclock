#include "alerts.h"
#include <algorithm>
#include <ESP8266HTTPClient.h>
#include "apps.h"
#include "diag.h"
#include "fonts.h"
#include "widgets.h"
#include "net.h"
#include "netreader.h"
#include "store.h"
#include "tlsclient.h"
#include "ukraine_map.h"

namespace alerts {

static const char *CONFIG = "/.sys/alerts.json";
static const int MIN_INTERVAL_S = 15;  // alerts.in.ua allows ~10 requests/min per IP

struct Config {
  String src = "klymenko";
  String token;
  int interval = 30;
  int home = -1;
  uint16_t clockColor = TFT_WHITE;
  int autoShow = 0;  // show the map for a minute when alerts change: 0 off, 1 anywhere, 2 home region
};

// "#RRGGBB" -> RGB565
static uint16_t parseColor(const String &hex, uint16_t def) {
  if (hex.length() != 7 || hex[0] != '#') return def;
  long v = strtol(hex.c_str() + 1, nullptr, 16);
  return ((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v & 0xFF) >> 3);
}

static Config cfg;
static Snapshot snap;
static uint32_t version = 0;   // bumped on each successful fetch
static time_t updatedAt = 0;   // UTC of last successful fetch
static char error[48] = "";  // last fetch error, guarded by lock
static volatile bool fetchNow = true;
static volatile bool paused = false;
static volatile uint32_t lastFetchAt = 0, lastFetchMs = 0, lastMapMs = 0;  // diagnostics

static TFT_eSprite *map8 = nullptr;
static uint32_t drawnVersion = UINT32_MAX;

static const int MAP_Y = 2, MAP_H = 160;  // matches BOX_Y/BOX_H in tools/gen_ukraine_map.py
static const int SCREEN_Y = MAP_Y + 2;    // the map is shown 2 px lower than generated
// Clock box: black area below Odesa and left of Crimea, see drawFooter.
static const int CLOCK_BOX_X1 = 148, CLOCK_BOX_Y0 = 147, CLOCK_Y = 168, CITY_Y = 215;

static void loadConfig() {
  Config c;
  c.src = store::getStr(CONFIG, "src", "klymenko");
  c.token = store::getStr(CONFIG, "token", "");
  c.interval = max(MIN_INTERVAL_S, store::getInt(CONFIG, "i", 30));
  c.home = constrain(store::getInt(CONFIG, "home", -1), -1, REGIONS - 1);
  c.clockColor = parseColor(store::getStr(CONFIG, "cc", "#ffffff"), TFT_WHITE);
  c.autoShow = constrain(store::getInt(CONFIG, "auto", 0), 0, 2);
  cfg = c;
}

static bool fetch(const Config &c, Snapshot &out, String &err) {
  String url;
  if (c.src == "ubilling") url = F("http://ubilling.net.ua/aerialalerts/");  // plain HTTP: no TLS memory
  else if (c.src == "neptun") url = F("https://neptun.in.ua/api/v1/alerts");
  else if (c.src == "alertsinua") {
    if (c.token.isEmpty()) return err = "немає токена", false;
    url = String(F("https://api.alerts.in.ua/v1/iot/active_air_raid_alerts_by_oblast.json?token=")) + c.token;
  } else url = F("https://vadimklimenko.com/map/statuses.json");

  // ubilling is plain HTTP: a plain client, no TLS buffers at all.
  bool secure = url.startsWith("https");
  std::unique_ptr<TlsClient> tls(secure ? new TlsClient() : nullptr);
  std::unique_ptr<WiFiClient> plain(secure ? nullptr : new WiFiClient());
  WiFiClient &client = secure ? (WiFiClient &)*tls : *plain;
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);  // plain body: JSON is parsed straight from the stream
  // Some sources reject unknown clients; present a desktop Chrome.
  http.setUserAgent(F("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36"));
  if (secure) tls->sizeFor(url);
  if (!http.begin(client, url)) return err = "немає з’єднання", false;
  int code = http.GET();
  if (code != 200) {
    err = secure ? tls->httpError(code) : "помилка HTTP " + String(code);
    http.end();
    return false;
  }
  bool ok;
  if (c.src == "alertsinua") {  // tiny: a 29-byte JSON string
    String body = http.getString();
    ok = parseAlertsInUa(body.c_str(), body.length(), out);
  } else {
    NetReader reader{http.getStream(), 10000};
    ok = c.src == "ubilling" ? parseWith(reader, filterUbilling, fromUbilling, out)
         : c.src == "neptun" ? parseWith(reader, filterNeptun, fromNeptun, out)
                             : parseKlymenkoStream(reader, out);
  }
  http.end();
  if (!ok) err = String("погані дані: ") + lastError;
  return ok;
}

static uint32_t last = 0;
bool poll() {
  if (paused || !settings.appEnabled[APP_ALERTS] || !net::connected()) return false;
  Config c;
  c = cfg;
  if (!fetchNow && millis() - last < (uint32_t)c.interval * 1000) return false;
  fetchNow = false;
  last = millis();
  lastFetchAt = last;

  Snapshot s;
  String err;
  net::fetchLock();
  diag::crumb(diag::FETCH, "alerts fetch");
  bool ok = fetch(c, s, err);
  net::fetchUnlock();
  Serial.printf("alerts: %s %s in %lu ms\n", c.src.c_str(), ok ? "ok" : err.c_str(), millis() - last);
  lastFetchMs = millis() - last;
  if (ok) {
    snap = s;
    updatedAt = time(nullptr);
    version++;
    error[0] = 0;
  } else {
    strlcpy(error, err.c_str(), sizeof error);
  }
  return true;
}

void begin() {
  loadConfig();
  memset(&snap, 0, sizeof snap);
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
  s = snap, upd = updatedAt;
  memcpy(err, error, sizeof err);
  c = cfg;
  char levels[REGIONS + 1], raions[RAIONS + 1];
  for (int i = 0; i < REGIONS; i++) levels[i] = "NPAU"[s.level[i]];
  levels[REGIONS] = 0;
  for (int i = 0; i < RAIONS; i++) raions[i] = "NYRI"[s.raion[i]];
  raions[RAIONS] = 0;
  return String("{\"fetchAt\":") + lastFetchAt + ",\"fetchMs\":" + lastFetchMs + ",\"mapMs\":" + lastMapMs + ",\"levels\":\"" + levels + "\",\"raions\":\"" + raions + "\",\"updated\":" + (long)upd + ",\"error\":\"" + err + "\",\"src\":\"" +
         c.src + "\",\"home\":" + c.home + "}";
}

// Alert colour of a region as the map shows it: 0 none, 1 orange (drones / some districts),
// 2 red (missiles / whole oblast), 3 no data. Raion colours win when the source has them, so
// "yellow in every raion" is orange, not red.
enum Severity : uint8_t { SEV_NONE = 0, SEV_ORANGE = 1, SEV_RED = 2, SEV_UNKNOWN = 3 };

static uint8_t severity(const Snapshot &s, int region) {
  if (region < 0) return SEV_NONE;
  if (s.level[region] == UNKNOWN) return SEV_UNKNOWN;
  bool raionData = false;
  uint8_t worst = R_NONE;
  for (int i = 0; i < MAP_RAIONS; i++) {
    if (mapRaion(i).oblast != region || s.raion[i] == R_INHERIT) continue;
    raionData = true;
    if (s.raion[i] > worst) worst = s.raion[i];
  }
  if (raionData && worst != R_NONE) return worst == R_RED ? SEV_RED : SEV_ORANGE;
  return s.level[region] == FULL ? SEV_RED : s.level[region] == PARTIAL ? SEV_ORANGE : SEV_NONE;
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

// The map is drawn in bands of BAND_ROWS map rows through a small 8-bit sprite (9.6 KB, only while
// drawing): one 38 KB sprite needs a contiguous block that a fragmented heap (TLS fetches) often lacks.
static const int BAND_ROWS = 40;
static int bandTop = 0;  // first map row of the band being drawn

// Even-odd scanline fill of rings [firstRing, firstRing + rings) into the band sprite.
// Vertices are in 1/MAP_FIX px; a pixel is filled when its centre lies inside (standard
// rasterisation rule), so shapes keep sub-pixel accuracy and neighbours share edges without gaps.
// Each edge is read from flash once per band: its crossings with the band's pixel-centre rows go
// into `cross` as (row << 16 | x), one sort orders them by row then x, and each row fills between
// pairs. (Walking every edge for every row read the 12k map points from flash ~6x as often.)
static const int CROSS_MAX = 512;  // the busiest region/band has 176 crossings
static uint32_t *cross = nullptr;  // CROSS_MAX entries, only while drawing

static void fillRings(int firstRing, int rings, uint16_t color) {
  const int F = MAP_FIX, H = MAP_FIX / 2;
  int top = MAP_Y + bandTop, bottom = min(MAP_Y + MAP_H, top + BAND_ROWS) - 1;
  int32_t yTop = top * F + H, yBottom = bottom * F + H;  // first/last pixel centre of the band
  int n = 0;
  for (int k = 0; k < rings; k++) {
    const MapRing ring = mapRing(firstRing + k);
    int16_t ax = mapPt(ring.first + ring.count - 1, 0), ay = mapPt(ring.first + ring.count - 1, 1);
    for (int i = 0; i < ring.count; i++) {
      int16_t bx = mapPt(ring.first + i, 0), by = mapPt(ring.first + i, 1);
      // centres yc with min(ay,by) <= yc < max(ay,by), as in the per-row test before
      int32_t lo = min(ay, by), hi = max(ay, by);
      if (hi > yTop && lo <= yBottom) {  // crosses a pixel-centre row of this band
        int32_t r0 = lo <= H ? 0 : (lo - H + F - 1) / F;  // first row whose centre >= lo
        for (int32_t r = max(r0, (int32_t)top); r <= bottom && r * F + H < hi; r++) {
          if (n >= CROSS_MAX) break;
          int32_t yc = r * F + H;
          int32_t x = ax + (yc - ay) * (bx - ax) / (by - ay);
          cross[n++] = (uint32_t)(r - top) << 16 | (uint16_t)constrain(x, 0, 0xFFFF);
        }
      }
      ax = bx, ay = by;
    }
    yield();
  }
  std::sort(cross, cross + n);
  for (int i = 0; i + 1 < n; i += 2) {
    int row = cross[i] >> 16;
    if ((int)(cross[i + 1] >> 16) != row) {  // odd count in a row (clipped list): resync
      i--;
      continue;
    }
    int32_t x0 = cross[i] & 0xFFFF, x1 = cross[i + 1] & 0xFFFF;
    int xa = (x0 - H + F - 1) / F, xb = (x1 - H) / F;  // first/last pixel centre inside
    if (xb >= xa) map8->drawFastHLine(xa, row, xb - xa + 1, color);
  }
}

// Oblast borders only (raion borders are not drawn).
static void outlineRegion(int r, uint16_t color) {
  const MapRegion reg = mapRegion(r);
  for (int k = 0; k < reg.rings; k++) {
    const MapRing ring = mapRing(reg.firstRing + k);
    for (int i = 0; i < ring.count; i++) {
      int ia = ring.first + i, ib = ring.first + (i + 1) % ring.count;
      const int16_t a[2] = {mapPt(ia, 0), mapPt(ia, 1)}, b[2] = {mapPt(ib, 0), mapPt(ib, 1)};
      int ya = a[1] / MAP_FIX - MAP_Y - bandTop, yb = b[1] / MAP_FIX - MAP_Y - bandTop;
      if ((ya < 0 && yb < 0) || (ya >= BAND_ROWS && yb >= BAND_ROWS)) continue;  // outside the band
      map8->drawLine(a[0] / MAP_FIX, ya, b[0] / MAP_FIX, yb, color);
    }
  }
}

static void drawMap(TFT_eSPI &tft, const Snapshot &s, int home) {
  map8 = new TFT_eSprite(&tft);
  map8->setColorDepth(8);
  cross = (uint32_t *)malloc(CROSS_MAX * sizeof(uint32_t));
  if (!cross || !map8->createSprite(240, BAND_ROWS)) {
    free(cross);
    cross = nullptr;
    delete map8;
    map8 = nullptr;
    return;
  }
  uint32_t t0 = millis();
  int clockRow = CLOCK_BOX_Y0 - SCREEN_Y;  // map rows from here on are pushed right of the clock box
  for (bandTop = 0; bandTop < MAP_H; bandTop += BAND_ROWS) {
    int rows = min(BAND_ROWS, MAP_H - bandTop);
    map8->fillSprite(TFT_BLACK);
    // Oblasts first (the only layer for sources without raion data), then raions on top,
    // then Kyiv city and Sevastopol, which sit inside other regions. Only oblast borders are drawn.
    for (int r = 0; r < REGIONS; r++)
      if (r != 9 && r != 18) fillRings(mapRegion(r).firstRing, mapRegion(r).rings, levelColor(s.level[r]));
    for (int i = 0; i < MAP_RAIONS; i++) {
      MapRaion ra = mapRaion(i);
      if (s.raion[i] != R_INHERIT && s.level[ra.oblast] != UNKNOWN) fillRings(ra.firstRing, ra.rings, raionColor(s.raion[i]));
    }
    for (int r : {9, 18}) fillRings(mapRegion(r).firstRing, mapRegion(r).rings, levelColor(s.level[r]));
    for (int r = 0; r < REGIONS; r++) outlineRegion(r, TFT_BLACK);
    if (home >= 0) outlineRegion(home, TFT_WHITE);
    // Around the clock box (map-free rows, see drawFooter): pushing it would erase the clock's top.
    int full = constrain(clockRow - bandTop, 0, rows);
    if (full) map8->pushSprite(0, SCREEN_Y + bandTop, 0, 0, 240, full);
    if (rows > full) map8->pushSprite(CLOCK_BOX_X1, SCREEN_Y + bandTop + full, CLOCK_BOX_X1, full, 240 - CLOCK_BOX_X1, rows - full);
  }
  bandTop = 0;
  lastMapMs = millis() - t0;
  free(cross);
  cross = nullptr;
  map8->deleteSprite();
  delete map8;
  map8 = nullptr;
}

static String hhmm(time_t utc) {
  struct tm t;
  localtime_r(&utc, &t);
  char b[8];
  strftime(b, sizeof b, "%H:%M", &t);
  return b;
}

// Clock in the black area below Odesa and left of Crimea (free of map pixels for screen rows 147..189,
// x < 148, checked against ukraine_map.h); home city on the bottom line in the largest font that
// fits it, coloured by its alert colour; a fetch problem, if any, small on the right.

// Each part is repainted only when its text or colour changed (a repaint clears its box first, which
// shows as a flicker); `full` after the screen was cleared.
static void drawFooter(TFT_eSPI &tft, const Snapshot &s, int home, time_t upd, const char *err, int interval,
                       uint16_t clockColor, bool full) {
  static String shownClock, shownCity, shownProblem;
  static uint16_t shownClockColor, shownCityColor;
  if (full) {
    tft.fillRect(0, SCREEN_Y + MAP_H, 240, 240 - SCREEN_Y - MAP_H, TFT_BLACK);
    tft.fillRect(0, CLOCK_BOX_Y0, CLOCK_BOX_X1, SCREEN_Y + MAP_H - CLOCK_BOX_Y0, TFT_BLACK);
    shownClock = shownCity = shownProblem = "";
  }

  String clock = net::timeValid() ? hhmm(time(nullptr)) : String("--:--");
  if (clock != shownClock || clockColor != shownClockColor) {
    if (!full) tft.fillRect(0, CLOCK_BOX_Y0, CLOCK_BOX_X1, 189 - CLOCK_BOX_Y0, TFT_BLACK);
    useFont(tft, FONT_ALERT_CLOCK);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(clockColor, TFT_BLACK);
    tft.drawString(clock, 3, CLOCK_Y);
    shownClock = clock, shownClockColor = clockColor;
  }

  String city = home >= 0 ? String(mapLabel(home)) : String("");
  uint16_t cityColor = TFT_DARKGREY;
  if (home >= 0) {
    uint8_t sev = severity(s, home);
    if (upd && sev != SEV_UNKNOWN) cityColor = sev == SEV_RED ? TFT_RED : sev == SEV_ORANGE ? TFT_ORANGE : TFT_GREEN;
  }
  if (city != shownCity || cityColor != shownCityColor) {
    if (!full) tft.fillRect(0, 190, 240, 50, TFT_BLACK);
    if (home >= 0) {
      useCityFont(tft, home);
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(cityColor, TFT_BLACK);
      tft.drawString(city, 3, CITY_Y);
    }
    shownCity = city, shownCityColor = cityColor;
  }

  bool stale = upd && time(nullptr) - upd > 3 * interval + 30;
  String problem;
  if (*err || stale) {
    useFont(tft, FONT_SMALL);
    problem = *err ? String(err) : "дані з " + hhmm(upd);
    if (tft.textWidth(problem) > 237 - CLOCK_BOX_X1) problem = *err ? "помилка" : "старі дані";
  }
  if (problem != shownProblem) {
    if (!full) tft.fillRect(CLOCK_BOX_X1, SCREEN_Y + MAP_H, 240 - CLOCK_BOX_X1, 190 - SCREEN_Y - MAP_H, TFT_BLACK);
    if (problem.length()) {
      useFont(tft, FONT_SMALL);
      tft.setTextDatum(MR_DATUM);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.drawString(problem, 237, 176);
    }
    shownProblem = problem;
  }
}

// Wi-Fi gone for OFFLINE_MS: the map turns grey (a short drop during reconnect is ignored).
static const uint32_t OFFLINE_MS = 15000;
static bool drawnOffline = false;

static bool isOffline() {
  static uint32_t lastOnline = 0;
  if (net::connected()) lastOnline = millis();
  return millis() - lastOnline > OFFLINE_MS;
}

static void render(TFT_eSPI &tft, bool full) {
  Snapshot s;
  uint32_t v;
  time_t upd;
  char err[sizeof error];
  int home, interval;
  uint16_t clockColor;
  s = snap, v = version, upd = updatedAt, home = cfg.home, interval = cfg.interval, clockColor = cfg.clockColor;
  memcpy(err, error, sizeof err);
  bool offline = isOffline();
  if (offline) {  // the last data may be outdated: no alert colours at all
    for (int r = 0; r < REGIONS; r++) s.level[r] = UNKNOWN;
    strlcpy(err, "без мережі", sizeof err);
  }
  if (full || v != drawnVersion || offline != drawnOffline) {
    drawnVersion = v, drawnOffline = offline;
    drawMap(tft, s, home);
  }
  drawFooter(tft, s, home, upd, err, interval, clockColor, full);
}

// ---- home alarm ---------------------------------------------------------------------

static const uint32_t BLINK_MS = 10000, BLINK_PHASE_MS = 500;
static uint32_t blinkStart = 0;
static uint8_t blinkLevel = NONE;
static bool blinking = false;

bool homeAlertStarted() {
  static uint32_t seenVersion = 0;
  static int seenHome = -2;
  static uint8_t prevSev = SEV_UNKNOWN, prevRaion[MAP_RAIONS];
  uint32_t v;
  int home;
  uint8_t sev, raion[MAP_RAIONS];
  v = version, home = cfg.home;
  sev = severity(snap, home);
  memcpy(raion, snap.raion, sizeof raion);
  if (v == seenVersion && home == seenHome) return false;
  bool first = seenVersion == 0 || home != seenHome;  // boot or home changed: just remember
  seenVersion = v, seenHome = home;
  // Any new alert message for the home region: a raion newly alerted, a raion yellow -> red, or
  // (sources without raion data) the region's alert appearing or growing.
  bool started = false;
  if (!first && v != 0 && home >= 0) {
    for (int i = 0; i < MAP_RAIONS; i++) {
      if (mapRaion(i).oblast != home || raion[i] == R_INHERIT || prevRaion[i] == R_INHERIT) continue;
      if ((prevRaion[i] == R_NONE && raion[i] != R_NONE) || (prevRaion[i] == R_YELLOW && raion[i] == R_RED)) started = true;
    }
    if ((sev == SEV_ORANGE || sev == SEV_RED) &&
        (prevSev == SEV_NONE || prevSev == SEV_UNKNOWN || (prevSev == SEV_ORANGE && sev == SEV_RED)))
      started = true;
  }
  prevSev = sev;
  memcpy(prevRaion, raion, sizeof prevRaion);
  if (started) {
    blinking = true;
    blinkStart = millis();
    blinkLevel = sev == SEV_RED ? FULL : PARTIAL;  // FULL = red frame, PARTIAL = orange frame
  }
  return started;
}

bool alertsChanged() {
  static uint32_t seenVersion = 0;
  static bool seen = false;
  static uint8_t prevLevel[REGIONS], prevRaion[MAP_RAIONS];
  uint32_t v = version;
  int mode = cfg.autoShow, home = cfg.home;
  bool any = false, homeChanged = false;
  if (v != seenVersion && v != 0) {
    if (seen) {
      for (int r = 0; r < REGIONS; r++)
        if (prevLevel[r] != snap.level[r]) any = true, homeChanged |= r == home;
      for (int i = 0; i < MAP_RAIONS; i++)
        if (prevRaion[i] != snap.raion[i]) any = true, homeChanged |= mapRaion(i).oblast == home;
    }
    memcpy(prevLevel, snap.level, sizeof prevLevel);
    memcpy(prevRaion, snap.raion, sizeof prevRaion);
    seen = true;
    seenVersion = v;
  }
  return mode == 1 ? any : mode == 2 ? homeChanged : false;
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
  // As large as fits: the word fills the width (narrowed font), the city uses its own largest font.
  useFont(tft, FONT_ALARM);
  tft.drawString("ТРИВОГА", 120, 92);  // full vs partial differs by colour only
  if (home >= 0) {
    useCityFont(tft, home);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(mapLabel(home), 120, 165);
  }
}

void hide() { drawnVersion = UINT32_MAX; }

bool blinkingNow() { return blinking; }

void draw(TFT_eSPI &tft) {
  tft.fillScreen(TFT_BLACK);
  render(tft, true);
  wifiMarkRedraw(tft);  // free corner: no map pixels right of x 162 in the top rows
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
    home = cfg.home;
    drawAlarmFrame(tft, home);
  } else {
    draw(tft);
  }
}

void tick(TFT_eSPI &tft) {
  if (blinking) return;  // loop() owns the screen while the alarm blinks
  static uint32_t lastFooter = 0;
  bool mapRedraw = drawnVersion != version || isOffline() != drawnOffline;
  if (mapRedraw || millis() - lastFooter > 1000) {  // cheap: the footer repaints only what changed
    lastFooter = millis();
    render(tft, false);
  }
  if (mapRedraw) wifiMarkRedraw(tft);  // the map sprite covers the corner
  else wifiMarkTick(tft);
}

}  // namespace alerts
