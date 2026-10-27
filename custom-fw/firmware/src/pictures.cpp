#include "pictures.h"
#include <AnimatedGIF.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <algorithm>
#include <vector>
#include "fonts.h"
#include "store.h"

namespace pictures {

static const char *DIR = "/image";
static const char *ALBUM = "/.sys/album.json";

static std::vector<String> files;
static bool listDirty = true;
static bool autoplay = false;
static int jpgSeconds = 30, gifLoops = 5;
static String selected;

static int current = -1;
static AnimatedGIF *gif = nullptr;  // allocated only while a GIF is shown
static int gifX = 0, gifY = 0, loopsDone = 0;
static uint32_t nextFrameAt = 0, shownAt = 0;
static TFT_eSPI *screen = nullptr;

// Playback diagnostics (/pic.json).
static uint32_t frames = 0, maxDecodeMs = 0, maxLateMs = 0, lateAt = 0, slowFrames = 0;

static bool isPicture(const String &name) {
  String n = name;
  n.toLowerCase();
  return n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".gif");
}

static bool isGif(const String &path) {
  String n = path;
  n.toLowerCase();
  return n.endsWith(".gif");
}

static void loadList() {
  files.clear();
  File d = LittleFS.open(DIR);
  if (d && d.isDirectory())
    for (File f = d.openNextFile(); f; f = d.openNextFile())
      if (!f.isDirectory() && isPicture(f.name())) files.push_back(String(DIR) + "/" + f.name());
  std::sort(files.begin(), files.end(), [](const String &a, const String &b) { return a < b; });

  JsonDocument album = store::load(ALBUM);
  autoplay = (album["autoplay"] | 0) == 1 || album["autoplay"] == "1";
  jpgSeconds = max(2, store::getInt(ALBUM, "i_i", 30));
  gifLoops = max(1, store::getInt(ALBUM, "gif_loop", 5));
  selected = store::getStr(ALBUM, "path", "");
  listDirty = false;
}

// ---- GIF: file callbacks over LittleFS, line drawing straight to the screen -----------

static void *gifOpen(const char *name, int32_t *size) {
  File *f = new File(LittleFS.open(name, "r"));
  if (!*f) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

static void gifClose(void *handle) {
  File *f = static_cast<File *>(handle);
  f->close();
  delete f;
}

static int32_t gifRead(GIFFILE *pFile, uint8_t *buf, int32_t len) {
  File *f = static_cast<File *>(pFile->fHandle);
  int32_t n = min(len, pFile->iSize - pFile->iPos);
  if (n <= 0) return 0;
  n = f->read(buf, n);
  pFile->iPos = f->position();
  return n;
}

static int32_t gifSeek(GIFFILE *pFile, int32_t pos) {
  File *f = static_cast<File *>(pFile->fHandle);
  f->seek(pos);
  pFile->iPos = f->position();
  return pFile->iPos;
}

// One line of a frame. Palette is big-endian RGB565, i.e. already in panel byte order.
// Opaque frames set the address window once (at the frame's first line) and then stream pixels:
// a window command per line made a lost command byte smear the rest of the frame.
static void gifDraw(GIFDRAW *d) {
  static uint16_t line[240];
  int top = gifY + d->iY, y = top + d->y, x0 = gifX + d->iX;
  int w = min(d->iWidth, 240 - x0);
  if (y < 0 || y >= 240 || w < 1) return;
  uint8_t *s = d->pPixels;
  if (d->ucDisposalMethod == 2) {  // restore to background
    for (int x = 0; x < w; x++)
      if (s[x] == d->ucTransparent) s[x] = d->ucBackground;
    d->ucHasTransparency = 0;
  }
  if (!d->ucHasTransparency) {
    for (int x = 0; x < w; x++) line[x] = d->pPalette[s[x]];
    if (d->y == 0) screen->setAddrWindow(x0, top, w, min(d->iHeight, 240 - top));
    screen->pushPixels(line, w);
    return;
  }
  // Transparent pixels keep what is on screen: push opaque runs only.
  for (int x = 0; x < w;) {
    while (x < w && s[x] == d->ucTransparent) x++;
    int start = x, n = 0;
    while (x < w && s[x] != d->ucTransparent) line[n++] = d->pPalette[s[x++]];
    if (n) screen->pushImage(x0 + start, y, n, 1, line);
  }
}

static void closeGif() {
  if (!gif) return;
  gif->close();
  delete gif;
  gif = nullptr;
}

// ---- JPG ------------------------------------------------------------------------------

static bool jpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= 240) return false;
  screen->pushImage(x, y, w, h, bitmap);
  return true;
}

static void message(TFT_eSPI &tft, const char *text) {
  tft.fillScreen(TFT_BLACK);
  useFont(tft, FONT_LARGE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(text, 120, 120);
}

static void open(TFT_eSPI &tft, int index) {
  closeGif();
  current = index;
  loopsDone = 0;
  shownAt = millis();
  const String &path = files[index];
  tft.fillScreen(TFT_BLACK);
  if (isGif(path)) {
    gif = new (std::nothrow) AnimatedGIF;
    if (!gif) return message(tft, "мало пам’яті");
    gif->begin(GIF_PALETTE_RGB565_BE);
    if (!gif->open(path.c_str(), gifOpen, gifClose, gifRead, gifSeek, gifDraw)) {
      closeGif();
      return message(tft, "помилка GIF");
    }
    gifX = max(0, (240 - gif->getCanvasWidth()) / 2);
    gifY = max(0, (240 - gif->getCanvasHeight()) / 2);
    nextFrameAt = 0;
  } else {
    uint16_t w = 0, h = 0;
    TJpgDec.getFsJpgSize(&w, &h, path, LittleFS);
    uint8_t scale = 1;
    while (scale < 8 && (w / scale > 240 || h / scale > 240)) scale *= 2;
    TJpgDec.setJpgScale(scale);
    TJpgDec.drawFsJpg((240 - w / scale) / 2, (240 - h / scale) / 2, path, LittleFS);
  }
}

static int startIndex() {
  if (!autoplay)
    for (size_t i = 0; i < files.size(); i++)
      if (files[i] == selected) return i;
  return current >= 0 && current < (int)files.size() ? current : 0;
}

void reload() { listDirty = true; }

void show(TFT_eSPI &tft) {
  screen = &tft;
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpgOutput);
  if (listDirty) loadList();
  if (files.empty()) return message(tft, "немає зображень");
  open(tft, startIndex());
}

void next(TFT_eSPI &tft) {
  if (files.empty()) return;
  open(tft, (current + 1) % files.size());
}

void loop(TFT_eSPI &tft) {
  if (listDirty) {
    show(tft);
    return;
  }
  if (files.empty()) return;
  if (gif) {
    if (millis() < nextFrameAt) return;
    int delayMs = 0;
    uint32_t t0 = millis(), late = nextFrameAt ? t0 - nextFrameAt : 0;
    if (late > maxLateMs) maxLateMs = late, lateAt = t0;
    tft.startWrite();
    int r = gif->playFrame(false, &delayMs);
    tft.endWrite();
    uint32_t dt = millis() - t0;
    frames++;
    if (dt > maxDecodeMs) maxDecodeMs = dt;
    if (dt > 200) slowFrames++;
    nextFrameAt = millis() + max(delayMs, 20);
    if (r == 0) {  // last frame of a loop
      loopsDone++;
      if (autoplay && files.size() > 1 && loopsDone >= gifLoops) return next(tft);
      gif->reset();
    } else if (r < 0) {
      if (autoplay && files.size() > 1) return next(tft);
      closeGif();
    }
  } else if (autoplay && files.size() > 1 && millis() - shownAt >= (uint32_t)jpgSeconds * 1000) {
    next(tft);
  }
}

void hide() { closeGif(); }

// Diagnostics: decode a GIF off-screen and CRC what gifDraw would push, per frame, to compare with
// a PC decode of the same file (tells corrupted reads from display problems).
static uint32_t crc;
static uint32_t crc32(uint32_t c, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  c = ~c;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320 : c >> 1;
  }
  return ~c;
}
static void crcDraw(GIFDRAW *d) {
  uint16_t line[240];
  int y = d->iY + d->y, x0 = d->iX, w = min(d->iWidth, 240 - x0);
  if (y < 0 || y >= 240 || w < 1) return;
  for (int x = 0; x < w; x++) line[x] = d->pPalette[d->pPixels[x]];
  int hdr[2] = {y, x0};
  crc = crc32(crc, hdr, sizeof hdr);
  crc = crc32(crc, line, w * 2);
}

String checkJson(const String &path, int count) {
  AnimatedGIF *g = new (std::nothrow) AnimatedGIF;
  if (!g) return "{\"error\":\"no memory\"}";
  g->begin(GIF_PALETTE_RGB565_LE);
  String out = "{\"crc\":[";
  if (g->open(path.c_str(), gifOpen, gifClose, gifRead, gifSeek, crcDraw)) {
    for (int f = 0; f < count; f++) {
      crc = 0;
      int d, r = g->playFrame(false, &d);
      out += String(f ? "," : "") + crc;
      if (r <= 0) break;
    }
    g->close();
  }
  delete g;
  return out + "]}";
}

String statsJson() {
  String s = String("{\"file\":\"") + (current >= 0 && current < (int)files.size() ? files[current] : String("")) +
             "\",\"frames\":" + frames + ",\"maxDecodeMs\":" + maxDecodeMs + ",\"slowFrames\":" + slowFrames +
             ",\"maxLateMs\":" + maxLateMs + ",\"lateAt\":" + lateAt + ",\"now\":" + millis() +
             ",\"heap\":" + ESP.getFreeHeap() + ",\"maxBlock\":" + ESP.getMaxAllocHeap() + "}";
  frames = maxDecodeMs = maxLateMs = slowFrames = 0;
  return s;
}

}  // namespace pictures
