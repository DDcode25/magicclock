#include "bitcoin.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <math.h>
#include <time.h>
#include "apps.h"
#include "diag.h"
#include "fonts.h"
#include "net.h"
#include "netreader.h"
#include "store.h"
#include "tlsclient.h"
#include "widgets.h"

namespace bitcoin {

static const int MAX_PAIRS = 12, CANDLES = 30;
static const char *API = "https://data-api.binance.vision/api/v3/";
static const char *COIN = "/.sys/coin2.json";
static const char *COLOR = "/.sys/ccolor.json";

struct Config {
  String sym[MAX_PAIRS], name[MAX_PAIRS];
  int count = 0;
  int theme = 0;  // 0 one pair + chart, 1 one pair large, 2 two pairs, 3 three pairs
  bool line = false, pctCandle = false, rotate = true, loading = true, clock = false;
  String kline = "1d";
  int refreshS = 30, rotateS = 10;
  uint16_t upFg, upBg, downFg, downBg, coinFg, coinBg, pctUpFg, pctUpBg, pctDownFg, pctDownBg;
  char dec = '.', thou = ',';
  uint16_t hourColor = TFT_WHITE, minuteColor = 0xFDC0;  // the clock app's (timecolor.json)
};

static const uint16_t C_INTERVAL = 0x3CDB;  // blue block with the candle interval (stock look)
static const uint16_t C_PLATE = 0x2A2E;     // dark blue plate under the clock

struct Candle {
  float o, h, l, c;
};

struct Pair {
  bool hasPrice = false, bad = false;  // bad: Binance does not know the pair
  double price = 0;
  float pct24 = 0;
  int candles = 0;
  Candle k[CANDLES];
  uint32_t klineAt = 0;  // millis of the candles, 0 = none
  String klineOf;        // their interval
};

static SemaphoreHandle_t lock;  // guards cfg, pairs, page, version, error
static Config cfg;
static Pair pairs[MAX_PAIRS];
static int page = 0;
static uint32_t version = 0;
static char error[48] = "";
static time_t updatedAt = 0;
static volatile bool fetchNow = true, paused = false, shown = false;

static int perPage(int theme) { return theme == 2 ? 2 : theme == 3 ? 3 : 1; }
static int pageCount(const Config &c) { return (c.count + perPage(c.theme) - 1) / perPage(c.theme); }

// "#RRGGBB" -> RGB565
static uint16_t parseColor(const String &hex, uint16_t def) {
  if (hex.length() != 7 || hex[0] != '#') return def;
  long v = strtol(hex.c_str() + 1, nullptr, 16);
  return ((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v & 0xFF) >> 3);
}

static uint16_t color(const char *key, uint16_t def) { return parseColor(store::getStr(COLOR, key, ""), def); }

static void loadConfig() {
  Config c;
  JsonDocument coin = store::load(COIN);
  for (int i = 0; i < MAX_PAIRS; i++) {
    String s = coin["cin" + String(i)] | "";
    s.trim();
    s.toUpperCase();
    if (s.isEmpty()) continue;
    c.sym[c.count] = s;
    c.name[c.count] = coin["cn" + String(i)] | "";
    c.count++;
  }
  c.refreshS = max(10, (int)String(coin["c_i"] | "30").toInt());
  c.rotate = String(coin["c_l"] | "1") == "1";
  c.loading = String(coin["c_loading"] | "1") == "1";
  c.clock = String(coin["c_clock"] | "0") == "1";
  c.pctCandle = String(coin["c_pct_mode"] | "0") == "1";
  c.rotateS = max(3, store::getInt("/.sys/cr.json", "rot_i", 10));
  c.kline = store::getStr("/.sys/k.json", "k", "1d");
  c.theme = constrain(store::getInt("/.sys/c_theme.json", "c_theme", 0), 0, 3);
  c.line = store::getInt("/.sys/c_chart.json", "c_chart", 0) == 1;
  c.upFg = color("up_fg", TFT_BLACK), c.upBg = color("up_bg", TFT_GREEN);
  c.downFg = color("down_fg", TFT_WHITE), c.downBg = color("down_bg", TFT_RED);
  c.coinFg = color("coin_fg", TFT_WHITE), c.coinBg = color("coin_bg", TFT_BLACK);
  c.pctUpFg = color("pct_up_fg", TFT_BLACK), c.pctUpBg = color("pct_up_bg", TFT_GREEN);
  c.pctDownFg = color("pct_down_fg", TFT_WHITE), c.pctDownBg = color("pct_down_bg", TFT_RED);
  String d = store::getStr("/.sys/sep.json", "decimal_sep", "."), t = store::getStr("/.sys/sep.json", "thousand_sep", ",");
  c.dec = d.length() ? d[0] : '.';
  c.thou = t.length() ? t[0] : 0;
  if (c.thou == c.dec) c.thou = 0;
  c.hourColor = parseColor(store::getStr("/.sys/timecolor.json", "hc", "#FFFFFF"), TFT_WHITE);
  c.minuteColor = parseColor(store::getStr("/.sys/timecolor.json", "mc", "#FEBA01"), 0xFDC0);

  xSemaphoreTake(lock, portMAX_DELAY);
  bool changed = c.count != cfg.count || c.kline != cfg.kline;
  for (int i = 0; i < c.count && !changed; i++) changed = c.sym[i] != cfg.sym[i];
  if (changed)
    for (Pair &p : pairs) p = Pair();
  cfg = c;
  if (page >= pageCount(c)) page = 0;
  version++;
  xSemaphoreGive(lock);
}

// ---- fetch ----------------------------------------------------------------------------

// GET url; 200 parses the body into doc (through filter when given). Returns the HTTP code, or -1.
static int getJson(const String &url, JsonDocument &doc, const JsonDocument *filter, String &err) {
  TlsClient client;
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);  // plain body: parsed straight from the stream
  if (!http.begin(client, url)) return err = "немає з’єднання", -1;
  int code = http.GET();
  if (code == 200) {
    NetReader r{http.getStream(), 10000};
    DeserializationError e = filter ? deserializeJson(doc, r, DeserializationOption::Filter(*filter)) : deserializeJson(doc, r);
    if (e) err = "погані дані", code = -1;
  } else {
    err = client.httpError(code);
  }
  http.end();
  return code;
}

static void tickerFilter(JsonDocument &f, bool array) {
  JsonObject o = array ? f[0].to<JsonObject>() : f.to<JsonObject>();
  o["symbol"] = true;
  o["lastPrice"] = true;
  o["priceChangePercent"] = true;
}

static void storeTicker(const Config &c, JsonObjectConst o) {
  String s = o["symbol"] | "";
  xSemaphoreTake(lock, portMAX_DELAY);
  for (int i = 0; i < c.count && i < cfg.count; i++)
    if (cfg.sym[i] == s) {
      pairs[i].price = atof(o["lastPrice"] | "0");
      pairs[i].pct24 = atof(o["priceChangePercent"] | "0");
      pairs[i].hasPrice = true;
      pairs[i].bad = false;
    }
  xSemaphoreGive(lock);
}

// All prices in one request; if Binance rejects it (an unknown pair), one request per pair.
static bool fetchTickers(const Config &c, String &err) {
  String list = "%5B";
  for (int i = 0; i < c.count; i++) list += (i ? ",%22" : "%22") + c.sym[i] + "%22";
  list += "%5D";
  JsonDocument filter, doc;
  tickerFilter(filter, true);
  int code = getJson(String(API) + "ticker/24hr?symbols=" + list, doc, &filter, err);
  if (code == 200) {
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) storeTicker(c, o);
    return true;
  }
  if (code != 400) return false;
  bool any = false;
  for (int i = 0; i < c.count; i++) {
    JsonDocument f1, d1;
    tickerFilter(f1, false);
    String e1;
    int code1 = getJson(String(API) + "ticker/24hr?symbol=" + c.sym[i], d1, &f1, e1);
    if (code1 == 200) storeTicker(c, d1.as<JsonObjectConst>()), any = true;
    else if (code1 == 400) {
      xSemaphoreTake(lock, portMAX_DELAY);
      if (i < cfg.count && cfg.sym[i] == c.sym[i]) pairs[i].bad = true;
      xSemaphoreGive(lock);
    }
  }
  if (any) err = "";
  return any;
}

// 30 candles: [[openTime, "open", "high", "low", "close", ...], ...]
static bool fetchKlines(const Config &c, int i, String &err) {
  JsonDocument doc;
  if (getJson(String(API) + "klines?symbol=" + c.sym[i] + "&interval=" + c.kline + "&limit=" + CANDLES, doc, nullptr, err) != 200)
    return false;
  Candle k[CANDLES];
  int n = 0;
  for (JsonArrayConst a : doc.as<JsonArrayConst>()) {
    if (n == CANDLES) break;
    k[n++] = {(float)atof(a[1] | "0"), (float)atof(a[2] | "0"), (float)atof(a[3] | "0"), (float)atof(a[4] | "0")};
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  if (i < cfg.count && cfg.sym[i] == c.sym[i]) {
    memcpy(pairs[i].k, k, sizeof k);
    pairs[i].candles = n;
    pairs[i].klineAt = millis();
    pairs[i].klineOf = c.kline;
  }
  xSemaphoreGive(lock);
  return true;
}

static bool needsCandles(const Config &c) { return c.theme == 0 || c.theme == 2 || c.pctCandle; }

static void task(void *) {
  uint32_t last = 0;
  bool lastOk = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    // Only while the app is on screen: the prices change every second and nobody sees them otherwise.
    if (paused || !shown || !settings.appEnabled[APP_BITCOIN] || !net::connected()) continue;
    Config c;
    int first;
    bool due[3] = {false, false, false};
    xSemaphoreTake(lock, portMAX_DELAY);
    c = cfg;
    first = page * perPage(c.theme);
    for (int j = 0; j < perPage(c.theme) && first + j < c.count; j++) {
      const Pair &p = pairs[first + j];
      due[j] = needsCandles(c) && !p.bad &&
               (!p.klineAt || p.klineOf != c.kline || millis() - p.klineAt >= (uint32_t)c.refreshS * 1000);
    }
    xSemaphoreGive(lock);
    if (!c.count) continue;
    bool tickers = fetchNow || !last || millis() - last >= (lastOk ? c.refreshS * 1000UL : 30000UL);
    if (!tickers && !due[0] && !due[1] && !due[2]) continue;

    String err;
    bool ok = true;
    net::fetchLock();
    diag::crumb(diag::FETCH, "btc fetch");
    if (tickers) {
      fetchNow = false;
      last = millis();
      ok = fetchTickers(c, err);
      lastOk = ok;
    }
    for (int j = 0; j < 3; j++)
      if (due[j] && !fetchKlines(c, first + j, err)) ok = false;
    net::fetchUnlock();
    Serial.printf("bitcoin: %s\n", ok ? "ok" : err.c_str());
    xSemaphoreTake(lock, portMAX_DELAY);
    if (ok) error[0] = 0, updatedAt = time(nullptr);
    else strlcpy(error, err.c_str(), sizeof error);
    version++;
    xSemaphoreGive(lock);
  }
}

void begin() {
  lock = xSemaphoreCreateMutex();
  loadConfig();
  xTaskCreatePinnedToCore(task, "bitcoin", 8192, nullptr, 1, nullptr, 0);
}

void reload() {
  loadConfig();
  fetchNow = true;
}

void pause(bool on) { paused = on; }

// ---- text -----------------------------------------------------------------------------

// Fixed decimals with the configured separators: 80846.63 -> "80,846.63".
static String number(double v, int decimals, const Config &c) {
  char raw[32];
  snprintf(raw, sizeof raw, "%.*f", decimals, fabs(v));
  const char *dot = strchr(raw, '.');
  int intLen = dot ? dot - raw : strlen(raw);
  String out = v < 0 ? "-" : "";
  for (int i = 0; i < intLen; i++) {
    if (i && c.thou && (intLen - i) % 3 == 0) out += c.thou;
    out += raw[i];
  }
  if (dot) out += c.dec, out += dot + 1;
  return out;
}

// Enough decimals for ~5 significant digits: 80846.63, 2.3456, 0.65432, 0.00001234.
static String price(double p, const Config &c) {
  int d = p > 0 ? 4 - (int)floor(log10(p)) : 2;
  return number(p, constrain(d, 2, 8), c);
}

static String percent(float v, const Config &c) { return (v >= 0 ? "+" : "") + number(v, 2, c) + "%"; }

// Custom name, else "BTC/USDT" for the common quote assets.
static String label(const Config &c, int i) {
  if (c.name[i].length()) return c.name[i];
  static const char *const QUOTES[] = {"FDUSD", "USDT", "USDC", "BUSD", "TUSD", "EUR", "TRY", "BTC", "ETH", "BNB"};
  for (const char *q : QUOTES) {
    int n = strlen(q);
    if (c.sym[i].length() > n && c.sym[i].endsWith(q)) return c.sym[i].substring(0, c.sym[i].length() - n) + "/" + q;
  }
  return c.sym[i];
}

// ---- drawing --------------------------------------------------------------------------
// The screen is composed in 30-row bands in a 16-bit sprite (14 KB, only while the app is shown) and
// pushed band by band: no flicker on updates. Layout code works in screen coordinates; OY is the top
// of the band being drawn. Without the sprite (no RAM) it draws straight to the screen.

static const int BAND_H = 30;  // 240 x 30 x 2 bytes: small next to a TLS fetch (~45 KB)
static TFT_eSprite *band = nullptr;
static int OY = 0;
static bool onSprite = false;

struct View {
  Config c;
  int first, n;
  Pair p[3];
  char err[48];
  bool stale;
};

static void text(TFT_eSPI &g, const String &s, int x, int y, uint8_t datum, uint16_t fg, uint16_t bg) {
  g.setTextDatum(datum);
  if (onSprite) g.setTextColor(fg);  // fg only: blends with what is already in the sprite
  else g.setTextColor(fg, bg);
  g.drawString(s, x, y - OY);
}

static void box(TFT_eSPI &g, int x, int y, int w, int h, uint16_t col) { g.fillRoundRect(x, y - OY, w, h, min(8, h / 4), col); }

// Largest of `fonts` (big to small) in which s fits `w`.
static void fit(TFT_eSPI &g, const String &s, int w, std::initializer_list<UiFont> fonts) {
  for (UiFont f : fonts) {
    useFont(g, f);
    if (g.textWidth(s) <= w) return;
  }
}

static float change(const View &v, int j) {
  const Pair &p = v.p[j];
  if (v.c.pctCandle && p.candles >= 2 && p.k[p.candles - 2].c > 0)
    return (p.price - p.k[p.candles - 2].c) / p.k[p.candles - 2].c * 100;
  return p.pct24;
}

// Name box (left) at y, height h.
static void nameBox(TFT_eSPI &g, const View &v, int j, int x, int y, int h, bool centre) {
  String s = label(v.c, v.first + j);
  useFont(g, FONT_LARGE);
  if (g.textWidth(s) > 150) useFont(g, FONT_SMALL);
  int w = g.textWidth(s) + 16;
  if (centre) x = 120 - w / 2;
  if (v.c.coinBg != TFT_BLACK) box(g, x, y, w, h, v.c.coinBg);
  text(g, s, x + 8, y + h / 2, ML_DATUM, v.c.coinFg, v.c.coinBg);
}

// Price in a box coloured by the change; status text instead while there is no price.
static void priceBox(TFT_eSPI &g, const View &v, int j, int y, int h, std::initializer_list<UiFont> fonts) {
  const Pair &p = v.p[j];
  if (!p.hasPrice || p.bad) {
    box(g, 4, y, 232, h, 0x2945);
    useFont(g, FONT_SMALL);
    String s = p.bad ? "невідома пара" : v.err[0] ? String(v.err) : v.c.loading ? "завантаження…" : "--";
    text(g, s, 120, y + h / 2, MC_DATUM, p.bad || v.err[0] ? TFT_YELLOW : TFT_LIGHTGREY, 0x2945);
    return;
  }
  bool up = change(v, j) >= 0;
  uint16_t bg = up ? v.c.upBg : v.c.downBg;
  String s = price(p.price, v.c);
  box(g, 4, y, 232, h, bg);
  fit(g, s, 220, fonts);
  text(g, s, 120, y + h / 2 + 1, MC_DATUM, up ? v.c.upFg : v.c.downFg, bg);
}

// Change in a box; x is its left edge, or its right edge with right = true. Returns the box width.
static int pctBox(TFT_eSPI &g, const View &v, int j, int x, int y, int h, UiFont font, bool right) {
  const Pair &p = v.p[j];
  if (!p.hasPrice || p.bad) return 0;
  float pc = change(v, j);
  bool up = pc >= 0;
  uint16_t bg = up ? v.c.pctUpBg : v.c.pctDownBg;
  String s = percent(pc, v.c);
  useFont(g, font);
  int w = g.textWidth(s) + 16;
  if (right) x -= w;
  box(g, x, y, w, h, bg);
  text(g, s, x + w / 2, y + h / 2 + 1, MC_DATUM, up ? v.c.pctUpFg : v.c.pctDownFg, bg);
  return w;
}

// Candles or a line over the last 30 candles in x0..x0+w, y0..y0+h.
static void chart(TFT_eSPI &g, const View &v, int j, int x0, int y0, int w, int h, bool line) {
  const Pair &p = v.p[j];
  if (p.candles < 2) return;
  float hi = -1e30f, lo = 1e30f;
  for (int i = 0; i < p.candles; i++) hi = max(hi, line ? p.k[i].c : p.k[i].h), lo = min(lo, line ? p.k[i].c : p.k[i].l);
  if (hi <= lo) hi = lo + 1;
  auto Y = [&](float val) { return y0 + (int)lroundf((hi - val) / (hi - lo) * (h - 1)) - OY; };
  float step = (float)w / CANDLES;
  if (line) {
    uint16_t col = p.k[p.candles - 1].c >= p.k[0].o ? v.c.upBg : v.c.downBg;
    for (int i = 1; i < p.candles; i++) {
      int xa = x0 + (int)((i - 1) * step + step / 2), xb = x0 + (int)(i * step + step / 2);
      g.drawLine(xa, Y(p.k[i - 1].c), xb, Y(p.k[i].c), col);
      g.drawLine(xa, Y(p.k[i - 1].c) + 1, xb, Y(p.k[i].c) + 1, col);
    }
    return;
  }
  int bw = max(1, (int)step - 2);
  for (int i = 0; i < p.candles; i++) {
    const Candle &k = p.k[i];
    uint16_t col = k.c >= k.o ? v.c.upBg : v.c.downBg;
    int x = x0 + (int)(i * step), xc = x + bw / 2;
    g.drawFastVLine(xc, Y(k.h), Y(k.l) - Y(k.h) + 1, col);
    int ya = Y(max(k.o, k.c)), yb = Y(min(k.o, k.c));
    g.fillRect(x, ya, bw, max(1, yb - ya + 1), col);
  }
}

static String clockText() {
  if (!net::timeValid()) return "--:--";
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char b[8];
  strftime(b, sizeof b, "%H:%M", &t);
  return b;
}

static void layout(TFT_eSPI &g, const View &v) {
  const Config &c = v.c;
  if (!c.count) {
    useFont(g, FONT_LARGE);
    text(g, "Додайте пари", 120, 105, MC_DATUM, TFT_LIGHTGREY, TFT_BLACK);
    useFont(g, FONT_SMALL);
    text(g, "у вкладці «Біткоїн»", 120, 135, MC_DATUM, TFT_DARKGREY, TFT_BLACK);
    return;
  }
  if (c.theme == 0) {  // as the stock screen: pair + change, price band with the interval, clock, candles
    const Pair &p = v.p[0];
    useFont(g, FONT_LARGE);
    String name = v.c.name[v.first].length() ? v.c.name[v.first] : v.c.sym[v.first];
    text(g, name, 6, 17, ML_DATUM, c.coinFg, TFT_BLACK);
    if (p.hasPrice && !p.bad) {
      float pc = change(v, 0);
      useFont(g, FONT_SMALL);
      text(g, percent(pc, c), 214, 17, MR_DATUM, pc >= 0 ? c.pctUpBg : c.pctDownBg, TFT_BLACK);
    }
    chart(g, v, 0, 4, 96, 232, 140, c.line);
    if (p.hasPrice && !p.bad) {  // price band, edge to edge
      bool up = change(v, 0) >= 0;
      uint16_t bg = up ? c.upBg : c.downBg;
      g.fillRect(0, 34 - OY, 182, 50, bg);
      String ps = price(p.price, c);
      fit(g, ps, 172, {FONT_PRICE_L, FONT_PRICE_M, FONT_PRICE_S});
      text(g, ps, 91, 60, MC_DATUM, up ? c.upFg : c.downFg, bg);
    } else {
      priceBox(g, v, 0, 34, 50, {FONT_PRICE_S});
    }
    g.fillRect(182, 34 - OY, 58, 50, C_INTERVAL);
    useFont(g, FONT_LARGE);
    text(g, c.kline, 211, 60, MC_DATUM, TFT_WHITE, C_INTERVAL);
    if (c.clock) {  // hours and minutes in the clock app's colours, on a small plate over the chart
      String t = clockText();
      useFont(g, FONT_LARGE);
      int w = g.textWidth(t) + 14;
      g.fillRoundRect(120 - w / 2, 88 - OY, w, 28, 5, C_PLATE);
      int x = 120 - g.textWidth(t) / 2;
      text(g, t.substring(0, 3), x, 102, ML_DATUM, c.hourColor, C_PLATE);
      text(g, t.substring(3), x + g.textWidth(t.substring(0, 3)), 102, ML_DATUM, c.minuteColor, C_PLATE);
    }
    if (p.candles) {
      useFont(g, FONT_SMALL);
      text(g, String(p.candles) + c.kline, 4, 230, ML_DATUM, TFT_WHITE, TFT_BLACK);
    }
  } else if (c.theme == 1) {  // large: name, price, change, clock
    nameBox(g, v, 0, 0, 14, 32, true);
    priceBox(g, v, 0, 58, 66, {FONT_PRICE_XL, FONT_PRICE_L, FONT_PRICE_M, FONT_PRICE_S});
    if (v.p[0].hasPrice && !v.p[0].bad) {
      useFont(g, FONT_PRICE_M);
      int w = g.textWidth(percent(change(v, 0), c)) + 16;
      pctBox(g, v, 0, 120 - w / 2, 134, 36, FONT_PRICE_M, false);
    }
    if (c.clock) {
      useFont(g, FONT_CLOCK_SEC);
      text(g, clockText(), 120, 208, MC_DATUM, TFT_WHITE, TFT_BLACK);
    }
  } else {  // 2 or 3 pairs stacked: name + change, price (+ line chart with 2 pairs)
    int rows = c.theme == 2 ? 2 : 3, rowH = 240 / rows;
    for (int j = 0; j < v.n; j++) {
      int y = j * rowH;
      nameBox(g, v, j, 4, y + 5, 28, false);
      pctBox(g, v, j, 236, y + 5, 28, FONT_PRICE_S, true);
      priceBox(g, v, j, y + 38, 36, {FONT_PRICE_M, FONT_PRICE_S});
      if (rows == 2) chart(g, v, j, 6, y + 80, 228, 34, true);
      if (j + 1 < rows) g.drawFastHLine(0, y + rowH - 1 - OY, 240, 0x2945);
    }
  }
  if (v.stale && v.err[0] && v.p[0].hasPrice) {  // old prices on screen: say why
    useFont(g, FONT_SMALL);
    text(g, "немає оновлень", 236, 230, MR_DATUM, TFT_YELLOW, TFT_BLACK);  // details in /btc.json
  }
}

static uint32_t drawnVersion = UINT32_MAX;
static int drawnMinute = -1;
static uint32_t pageAt = 0;

static void render(TFT_eSPI &tft) {
  static View v;  // ~2 KB: off the UI task stack
  xSemaphoreTake(lock, portMAX_DELAY);
  v.c = cfg;
  v.first = page * perPage(cfg.theme);
  v.n = min(perPage(cfg.theme), cfg.count - v.first);
  for (int j = 0; j < v.n; j++) v.p[j] = pairs[v.first + j];
  memcpy(v.err, error, sizeof v.err);
  v.stale = !updatedAt || time(nullptr) - updatedAt > 3 * cfg.refreshS + 30;
  drawnVersion = version;
  xSemaphoreGive(lock);

  if (!band) {
    band = new TFT_eSprite(&tft);
    band->setColorDepth(16);
    if (!band->createSprite(240, BAND_H)) {
      delete band;
      band = nullptr;
    }
  }
  diag::crumb(diag::UI, "btc render");
  if (band) {
    band->setTextWrap(false, false);
    onSprite = true;
    for (OY = 0; OY < 240; OY += BAND_H) {
      band->fillSprite(TFT_BLACK);
      layout(*band, v);
      band->pushSprite(0, OY);
    }
  } else {
    onSprite = false;
    OY = 0;
    tft.fillScreen(TFT_BLACK);
    layout(tft, v);
  }
  OY = 0;
  if (v.c.theme <= 1) wifiMarkRedraw(tft);
  diag::crumb(diag::UI, "btc idle");  // stacked pairs use the corner
}

void draw(TFT_eSPI &tft) {
  shown = true;
  fetchNow = true;
  pageAt = millis();
  drawnMinute = -1;
  render(tft);
}

void tick(TFT_eSPI &tft) {
  bool redraw = drawnVersion != version;
  xSemaphoreTake(lock, portMAX_DELAY);
  int pages = pageCount(cfg);
  bool rotate = cfg.rotate && pages > 1 && millis() - pageAt >= (uint32_t)cfg.rotateS * 1000;
  if (rotate) page = (page + 1) % pages, pageAt = millis();
  bool clock = cfg.clock && cfg.theme <= 1;
  int theme = cfg.theme;
  xSemaphoreGive(lock);
  if (clock && net::timeValid()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_min != drawnMinute) drawnMinute = t.tm_min, redraw = true;
  }
  if (redraw || rotate) render(tft);
  else if (theme <= 1) wifiMarkTick(tft);
}

void next(TFT_eSPI &tft) {
  xSemaphoreTake(lock, portMAX_DELAY);
  int pages = pageCount(cfg);
  if (pages > 1) page = (page + 1) % pages;
  pageAt = millis();
  xSemaphoreGive(lock);
  render(tft);
}

void hide() {
  shown = false;
  if (band) {
    band->unloadFont();
    band->deleteSprite();
    delete band;
    band = nullptr;
  }
}

String statusJson() {
  JsonDocument doc;
  xSemaphoreTake(lock, portMAX_DELAY);
  doc["page"] = page;
  doc["error"] = error;
  doc["updated"] = (long)updatedAt;
  for (int i = 0; i < cfg.count; i++) {
    JsonObject o = doc["pairs"].add<JsonObject>();
    o["symbol"] = cfg.sym[i];
    o["price"] = pairs[i].price;
    o["pct24"] = pairs[i].pct24;
    o["candles"] = pairs[i].candles;
    o["bad"] = pairs[i].bad;
  }
  xSemaphoreGive(lock);
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace bitcoin
