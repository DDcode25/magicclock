#include "diag.h"
#include <ArduinoJson.h>
#include <user_interface.h>
#include "net.h"
#include "store.h"

namespace diag {

static const char *LOG = "/.sys/boots.json";
static const uint32_t MAGIC = 0x4D424657;  // "MBFW": the RTC breadcrumbs are valid

// Kept in RTC user memory, which survives a crash or watchdog reset (not a power cycle).
struct Rtc {
  uint32_t magic;
  char last[3][32];
  uint32_t lowHeap;
  uint32_t crashes;  // crash / watchdog resets in a row without STABLE_MS of uptime in between
};
static const uint32_t STABLE_MS = 120000;
static Rtc rtc;
static const uint32_t RTC_OFFSET = 32;  // words; the start of user memory is used by the OTA code

static void save() { ESP.rtcUserMemoryWrite(RTC_OFFSET, (uint32_t *)&rtc, sizeof rtc); }

void begin() {
  const rst_info *ri = ESP.getResetInfoPtr();
  ESP.rtcUserMemoryRead(RTC_OFFSET, (uint32_t *)&rtc, sizeof rtc);
  bool crashed = ri->reason == REASON_EXCEPTION_RST || ri->reason == REASON_SOFT_WDT_RST || ri->reason == REASON_WDT_RST;
  if (rtc.magic != MAGIC) rtc.crashes = 0;
  rtc.crashes = crashed ? rtc.crashes + 1 : 0;
  JsonDocument doc = store::load(LOG);
  JsonArray list = doc["boots"].is<JsonArray>() ? doc["boots"].as<JsonArray>() : doc["boots"].to<JsonArray>();
  JsonObject e = list.add<JsonObject>();
  e["reason"] = ESP.getResetReason();
  e["fw"] = FW_VERSION;
  if (crashed) {
    char b[80];
    snprintf(b, sizeof b, "exc %u epc1 0x%08x excvaddr 0x%08x", ri->exccause, ri->epc1, ri->excvaddr);
    e["panic"] = b;
  }
  if (ri->reason != REASON_DEFAULT_RST && rtc.magic == MAGIC) {
    e["ui"] = (const char *)rtc.last[UI];
    e["web"] = (const char *)rtc.last[WEB];
    e["fetch"] = (const char *)rtc.last[FETCH];
    e["minHeap"] = rtc.lowHeap;
  }
  while (list.size() > 10) list.remove(0);
  store::save(LOG, doc);
  rtc.magic = MAGIC;
  for (auto &l : rtc.last) strlcpy(l, "boot", sizeof l);
  rtc.lowHeap = ESP.getFreeHeap();
  save();
}

bool safeMode() { return rtc.magic == MAGIC && rtc.crashes >= 3; }

void loop() {
  if (rtc.crashes && millis() > STABLE_MS) {
    rtc.crashes = 0;
    save();
  }
}

void crumb(Slot slot, const char *what) {
  strlcpy(rtc.last[slot], what, sizeof rtc.last[slot]);
  uint32_t h = ESP.getFreeHeap();
  if (h < rtc.lowHeap) rtc.lowHeap = h;
  save();
}

struct Kept {
  char what[24];
  int32_t bytes;
  uint32_t at;  // uptime s
};
static Kept kept[8];
static int keptCount = 0;

void fetchDone(uint32_t heapBefore) {
  int32_t d = (int32_t)heapBefore - (int32_t)ESP.getFreeHeap();
  if (d < 4000) return;
  Kept &k = kept[keptCount++ % 8];
  strlcpy(k.what, rtc.last[FETCH], sizeof k.what);
  k.bytes = d;
  k.at = millis() / 1000;
}

String json() {
  JsonDocument doc;
  doc["uptime"] = millis() / 1000;
  doc["heap"] = ESP.getFreeHeap();
  doc["minHeap"] = rtc.lowHeap;
  doc["maxBlock"] = ESP.getMaxFreeBlockSize();
  doc["frag"] = ESP.getHeapFragmentation();
  doc["reset"] = ESP.getResetReason();
  doc["safeMode"] = safeMode();
  doc["ui"] = (const char *)rtc.last[UI];
  doc["web"] = (const char *)rtc.last[WEB];
  doc["fetch"] = (const char *)rtc.last[FETCH];
  for (int i = max(0, keptCount - 8); i < keptCount; i++) {
    JsonObject o = doc["kept"].add<JsonObject>();
    o["what"] = (const char *)kept[i % 8].what;
    o["bytes"] = kept[i % 8].bytes;
    o["at"] = kept[i % 8].at;
  }
  doc["boots"] = store::load(LOG)["boots"];
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace diag
