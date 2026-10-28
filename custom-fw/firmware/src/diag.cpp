#include "diag.h"
#include <ArduinoJson.h>
#include <esp_debug_helpers.h>
#include <esp_private/panic_internal.h>
#include <esp_system.h>
#include <freertos/xtensa_context.h>
#include "net.h"
#include "store.h"

namespace diag {

static const char *LOG = "/.sys/boots.json";
static const uint32_t MAGIC = 0x4D424657;  // "MBFW": the RTC breadcrumb is valid
RTC_NOINIT_ATTR static uint32_t magic;
RTC_NOINIT_ATTR static char last[3][32];
RTC_NOINIT_ATTR static uint32_t lowHeap;  // lowest free heap seen by crumb()
// Filled by the panic handler: reason, task, backtrace (decode with addr2line on firmware.elf).
static const int PCS = 10;
RTC_NOINIT_ATTR static uint32_t panicMagic;
RTC_NOINIT_ATTR static char panicReason[40], panicTask[16];
RTC_NOINIT_ATTR static uint32_t panicPc[PCS];
RTC_NOINIT_ATTR static int panicCore;

static const char *reasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
  }
}

void begin() {
  esp_reset_reason_t r = esp_reset_reason();
  JsonDocument doc = store::load(LOG);
  JsonArray list = doc["boots"].is<JsonArray>() ? doc["boots"].as<JsonArray>() : doc["boots"].to<JsonArray>();
  JsonObject e = list.add<JsonObject>();
  e["reason"] = reasonText(r);
  e["fw"] = FW_VERSION;
  if (r != ESP_RST_POWERON && magic == MAGIC) {
    e["ui"] = (const char *)last[UI];
    e["web"] = (const char *)last[WEB];
    e["fetch"] = (const char *)last[FETCH];
    e["minHeap"] = lowHeap;
  }
  if (panicMagic == MAGIC) {
    e["panic"] = (const char *)panicReason;
    e["task"] = (const char *)panicTask;
    e["core"] = panicCore;
    String bt;
    for (int i = 0; i < PCS && panicPc[i]; i++) {
      char b[12];
      snprintf(b, sizeof b, "%s0x%08x", i ? " " : "", (unsigned)panicPc[i]);
      bt += b;
    }
    e["bt"] = bt;
    panicMagic = 0;
  }
  while (list.size() > 10) list.remove(0);
  store::save(LOG, doc);
  magic = MAGIC;
  for (auto &l : last) strlcpy(l, "boot", sizeof l);
  lowHeap = ESP.getFreeHeap();
}

void crumb(Slot slot, const char *what) {
  strlcpy(last[slot], what, sizeof last[slot]);
  uint32_t h = ESP.getMinFreeHeap();
  if (h < lowHeap) lowHeap = h;
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
  if (d < 8000) return;
  Kept &k = kept[keptCount++ % 8];
  strlcpy(k.what, last[FETCH], sizeof k.what);
  k.bytes = d;
  k.at = millis() / 1000;
}

String json() {
  JsonDocument doc;
  doc["uptime"] = millis() / 1000;
  doc["heap"] = ESP.getFreeHeap();
  doc["minHeap"] = ESP.getMinFreeHeap();
  doc["maxBlock"] = ESP.getMaxAllocHeap();
  doc["reset"] = reasonText(esp_reset_reason());
  doc["tlsReserve"] = net::tlsReserved();
  doc["ui"] = (const char *)last[UI];
  doc["web"] = (const char *)last[WEB];
  doc["fetch"] = (const char *)last[FETCH];
  for (int i = max(0, keptCount - 8); i < keptCount; i++) {
    JsonObject o = doc["kept"].add<JsonObject>();
    o["what"] = (const char *)kept[i % 8].what;
    o["bytes"] = kept[i % 8].bytes;
    o["at"] = kept[i % 8].at;
  }
  // Background tasks: state (0 running, 1 ready, 2 blocked, 3 suspended, 4 deleted) and free stack words.
  for (const char *name : {"alerts", "weather", "bitcoin", "outage", "web"}) {
    TaskHandle_t t = xTaskGetHandle(name);
    JsonObject o = doc["tasks"][name].to<JsonObject>();
    if (!t) {
      o["state"] = -1;
      continue;
    }
    o["state"] = (int)eTaskGetState(t);
    o["stack"] = uxTaskGetStackHighWaterMark(t);
  }
  doc["boots"] = store::load(LOG)["boots"];
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace diag

// Runs inside the panic handler (flash cache may be off): IRAM, no library calls but the ROM-safe
// backtrace helpers. Records where it crashed, then the normal handler prints and reboots.
extern "C" void __real_esp_panic_handler(panic_info_t *info);
extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info) {
  using namespace diag;
  const char *r = info->reason ? info->reason : "?";
  int i = 0;
  for (; r[i] && i < (int)sizeof panicReason - 1; i++) panicReason[i] = r[i];
  panicReason[i] = 0;
  const char *t = pcTaskGetName(xTaskGetCurrentTaskHandleForCPU(info->core));
  for (i = 0; t && t[i] && i < (int)sizeof panicTask - 1; i++) panicTask[i] = t[i];
  panicTask[i] = 0;
  panicCore = info->core;
  for (i = 0; i < PCS; i++) panicPc[i] = 0;
  if (info->frame) {
    const XtExcFrame *f = (const XtExcFrame *)info->frame;
    esp_backtrace_frame_t bt = {.pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = nullptr};
    panicPc[0] = esp_cpu_process_stack_pc(bt.pc);
    for (i = 1; i < PCS && bt.next_pc; i++) {
      if (!esp_backtrace_get_next_frame(&bt)) break;
      panicPc[i] = esp_cpu_process_stack_pc(bt.pc);
    }
  }
  panicMagic = MAGIC;
  __real_esp_panic_handler(info);
}
