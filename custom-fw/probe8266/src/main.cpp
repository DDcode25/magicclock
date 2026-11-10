// SD PRO (ESP8266) probe firmware.
// Never mounts or writes LittleFS; the only flash writes are OTA (/update).
//
//   GET  /               info: chip, flash, sketch, heap, current display candidate
//   GET  /flash          raw flash dump (?offset=0&size=<bytes>, default whole chip)
//   POST /update         OTA, multipart field "firmware" (ESP8266HTTPUpdateServer)
//   GET  /disp?i=N       stop cycling, show display candidate N
//   GET  /disp?dc=&rst=&cs=&bl=&blinv=   stop cycling, try a custom pin set
//   GET  /seq            pin-confirmation sequence: stages 1..4, 5 s each, repeated
//   GET  /cycle          resume cycling through candidates (4 s each)
//   GET  /reboot
//
// Display: assumed ST7789 on hardware SPI (SCLK GPIO14, MOSI GPIO13), SPI mode 3.
// DC/RST/CS/backlight are unknown, so the probe cycles through candidate pin sets and
// paints the candidate number big on the screen. Whichever number shows up is the answer.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <SPI.h>
#include "secrets.h"

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer updater;

struct Cand { int8_t dc, rst, cs, bl; bool blLow; };

// Most likely first (GeekMagic-style ESP8266 clocks: DC 0, RST 2, no CS, BL 5 active low).
static Cand cands[64];
static int nCands = 0;
static int cur = 0;
static bool cycling = true;
static uint32_t lastSwitch = 0;
static Cand custom;
static int seqStage = 0;  // 0 = off
static bool useCustom = false;

static void addCand(int8_t dc, int8_t rst, int8_t cs, int8_t bl, bool blLow) {
  if (nCands < 64) cands[nCands++] = {dc, rst, cs, bl, blLow};
}

static void buildCands() {
  const int8_t bls[] = {5, 4, 12, 16};
  // Tier 1: the common layout with every backlight variant.
  for (int8_t bl : bls) { addCand(0, 2, -1, bl, true); addCand(0, 2, -1, bl, false); }
  // Tier 2: other DC/RST pairs, backlight on GPIO5 both polarities, with and without CS 15.
  const int8_t pairs[][2] = {{2, 0}, {0, -1}, {2, -1}, {4, 2}, {5, 2}, {0, 16}, {16, 2}, {4, 5}, {5, 4}, {15, 2}, {2, 4}, {0, 4}};
  for (auto &p : pairs) {
    int8_t bl = 5;
    for (int8_t b : {5, 4, 12}) if (b != p[0] && b != p[1]) { bl = b; break; }
    addCand(p[0], p[1], -1, bl, true);
    addCand(p[0], p[1], -1, bl, false);
  }
  addCand(0, 2, 15, 5, true);
  addCand(0, 2, 15, 5, false);
  addCand(2, 4, 15, 5, true);
  addCand(2, 4, 15, 5, false);
}

// ---------------- ST7789 ----------------
static Cand C;

static inline void csLow() { if (C.cs >= 0) digitalWrite(C.cs, LOW); }
static inline void csHigh() { if (C.cs >= 0) digitalWrite(C.cs, HIGH); }

static void cmd(uint8_t c) { digitalWrite(C.dc, LOW); csLow(); SPI.write(c); csHigh(); digitalWrite(C.dc, HIGH); }
static void data(uint8_t d) { csLow(); SPI.write(d); csHigh(); }

static void window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  cmd(0x2A); data(x0 >> 8); data(x0); data(x1 >> 8); data(x1);
  cmd(0x2B); data(y0 >> 8); data(y0); data(y1 >> 8); data(y1);
  cmd(0x2C);
}

static void fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
  window(x, y, x + w - 1, y + h - 1);
  csLow();
  uint8_t hi = color >> 8, lo = color;
  for (uint32_t i = 0, n = (uint32_t)w * h; i < n; i++) { SPI.write(hi); SPI.write(lo); if ((i & 4095) == 0) yield(); }
  csHigh();
}

// 3x5 digit font, one row per nibble-ish (bits 2..0 = columns left..right).
static const uint8_t DIGITS[10][5] = {
  {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
  {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};

static void drawDigit(int d, int x, int y, int s, uint16_t color) {
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 3; c++)
      if (DIGITS[d][r] & (4 >> c)) fill(x + c * s, y + r * s, s, s, color);
}

static uint32_t holdHighMask = 0;  // pins releasePins() drives high instead of floating

static void releasePins() {
  const int8_t pins[] = {0, 2, 4, 5, 12, 15, 16};
  for (int8_t p : pins) {
    if (holdHighMask & (1u << p)) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
    else pinMode(p, INPUT);
  }
  if (holdHighMask) delay(150);  // let a just-released reset line finish the controller's reset
}

static void showCandidate(const Cand &c, int number) {
  C = c;
  releasePins();
  // Backlight: drive the chosen pin "on".
  pinMode(c.bl, OUTPUT);
  digitalWrite(c.bl, c.blLow ? LOW : HIGH);
  pinMode(c.dc, OUTPUT);
  digitalWrite(c.dc, HIGH);
  if (c.cs >= 0) { pinMode(c.cs, OUTPUT); digitalWrite(c.cs, HIGH); }
  if (c.rst >= 0) {
    pinMode(c.rst, OUTPUT);
    digitalWrite(c.rst, HIGH); delay(5);
    digitalWrite(c.rst, LOW); delay(20);
    digitalWrite(c.rst, HIGH); delay(150);
  }
  cmd(0x01); delay(150);          // SWRESET
  cmd(0x11); delay(120);          // SLPOUT
  cmd(0x3A); data(0x55);          // 16 bpp
  cmd(0x36); data(0x00);          // MADCTL
  cmd(0x21);                      // INVON (typical for IPS ST7789)
  cmd(0x13);                      // NORON
  cmd(0x29); delay(20);           // DISPON

  static const uint16_t bg[] = {0x001F, 0x07E0, 0xF800, 0xFFE0, 0xF81F, 0x07FF};
  fill(0, 0, 240, 240, bg[number % 6]);
  fill(20, 70, 200, 100, 0x0000);
  int tens = number / 10, ones = number % 10, s = 16;
  if (number >= 10) drawDigit(tens, 120 - 3 * s - s / 2 - s, 80, s, 0xFFFF);
  drawDigit(ones, number >= 10 ? 120 + s / 2 : 120 - 3 * s / 2, 80, s, 0xFFFF);
}

// Confirmation sequence on top of the found layout (DC 0, CS 15, BL 5 active low).
// Every stage starts clean: GPIO2 and GPIO4 driven high, full init, number painted.
// 1.5 s later the stage applies its action and keeps it until the next stage:
//  1: nothing                     -> number stays the whole 5 s
//  2: GPIO2 low                   -> picture vanishes if RST is GPIO2
//  3: GPIO4 low                   -> picture vanishes if RST is GPIO4
//  4: GPIO5 high                  -> screen goes dark if BL is active low
static void runStage(int st) {
  Cand base = {0, -1, 15, 5, true};
  holdHighMask = (1u << 2) | (1u << 4);
  showCandidate(base, st);
}

static void stageAction(int st) {
  if (st == 2) digitalWrite(2, LOW);
  if (st == 3) digitalWrite(4, LOW);
  if (st == 4) digitalWrite(5, HIGH);
}

// ---------------- Web ----------------
static String candStr(const Cand &c) {
  char b[80];
  snprintf(b, sizeof b, "dc=%d rst=%d cs=%d bl=%d bl_active=%s", c.dc, c.rst, c.cs, c.bl, c.blLow ? "low" : "high");
  return b;
}

static void handleRoot() {
  String o = "SD PRO probe\n\n";
  o += "MAC: " + WiFi.macAddress() + "\n";
  o += "Chip ID: " + String(ESP.getChipId(), HEX) + "\n";
  o += "Core: " + ESP.getCoreVersion() + ", SDK " + ESP.getSdkVersion() + "\n";
  o += "Flash ID: " + String(ESP.getFlashChipId(), HEX) + ", real size " + ESP.getFlashChipRealSize() +
       ", header size " + ESP.getFlashChipSize() + ", " + ESP.getFlashChipSpeed() / 1000000 + " MHz\n";
  o += "Sketch: " + String(ESP.getSketchSize()) + " used, " + ESP.getFreeSketchSpace() + " free for OTA\n";
  o += "Heap: " + String(ESP.getFreeHeap()) + " free, max block " + ESP.getMaxFreeBlockSize() + "\n";
  o += "Reset: " + ESP.getResetReason() + "\n";
  o += "Uptime: " + String(millis() / 1000) + " s\n\n";
  o += "Display: " + String(cycling ? "cycling" : "fixed") + ", showing #" + cur + " " +
       candStr(useCustom ? custom : cands[cur]) + "\n\nCandidates:\n";
  for (int i = 0; i < nCands; i++) o += "  #" + String(i) + " " + candStr(cands[i]) + "\n";
  server.send(200, "text/plain", o);
}

static void handleFlash() {
  uint32_t total = ESP.getFlashChipRealSize();
  uint32_t off = server.hasArg("offset") ? strtoul(server.arg("offset").c_str(), nullptr, 0) : 0;
  uint32_t size = server.hasArg("size") ? strtoul(server.arg("size").c_str(), nullptr, 0) : total - off;
  off &= ~3u; size &= ~3u;
  if (off >= total || size == 0 || off + size > total) { server.send(400, "text/plain", "bad range"); return; }
  static uint32_t buf[1024];
  server.setContentLength(size);
  server.send(200, "application/octet-stream", "");
  WiFiClient client = server.client();
  for (uint32_t done = 0; done < size;) {
    uint32_t n = min<uint32_t>(sizeof buf, size - done);
    if (!ESP.flashRead(off + done, buf, n)) break;
    if (client.write((const uint8_t *)buf, n) != n) break;
    done += n;
    yield();
  }
}

static int8_t argPin(const char *name, int8_t def) {
  return server.hasArg(name) ? (int8_t)server.arg(name).toInt() : def;
}

static void handleDisp() {
  cycling = false;
  seqStage = 0;
  holdHighMask = 0;
  if (server.hasArg("i")) {
    useCustom = false;
    cur = constrain(server.arg("i").toInt(), 0, nCands - 1);
    showCandidate(cands[cur], cur);
  } else {
    useCustom = true;
    custom = {argPin("dc", 0), argPin("rst", 2), argPin("cs", -1), argPin("bl", 5), server.arg("blinv") != "0"};
    showCandidate(custom, 99);
  }
  handleRoot();
}

void setup() {
  Serial.begin(115200);
  buildCands();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname("sdpro-probe");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    // Fallback: open AP at 192.168.4.1 while STA keeps retrying.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("SDPro-Probe");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  updater.setup(&server, "/update");
  server.on("/", handleRoot);
  server.on("/flash", handleFlash);
  server.on("/disp", handleDisp);
  server.on("/seq", [] { cycling = false; seqStage = 1; runStage(1); lastSwitch = millis(); handleRoot(); });
  server.on("/cycle", [] { seqStage = 0; cycling = true; useCustom = false; lastSwitch = 0; handleRoot(); });
  server.on("/reboot", [] { server.send(200, "text/plain", "rebooting"); delay(200); ESP.restart(); });
  server.begin();

  SPI.begin();
  SPI.setFrequency(20000000);
  SPI.setDataMode(SPI_MODE3);
  cur = 0;
  showCandidate(cands[0], 0);
  lastSwitch = millis();
}

void loop() {
  server.handleClient();
  // Without the station link, keep the fallback AP up; drop it once connected.
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 5000) {
    lastCheck = millis();
    if (WiFi.status() == WL_CONNECTED && (WiFi.getMode() & WIFI_AP)) WiFi.mode(WIFI_STA);
  }
  static bool acted = false;
  if (seqStage && !acted && millis() - lastSwitch > 1500) { stageAction(seqStage); acted = true; }
  if (seqStage && millis() - lastSwitch > 5000) {
    acted = false;
    seqStage = seqStage % 4 + 1;
    runStage(seqStage);
    lastSwitch = millis();
  }
  if (cycling && millis() - lastSwitch > 4000) {
    cur = (cur + 1) % nCands;
    showCandidate(cands[cur], cur);
    lastSwitch = millis();
  }
}
