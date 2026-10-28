#pragma once
#include <Arduino.h>

// Crash diagnostics. A breadcrumb (what the firmware was doing) survives a panic or watchdog reset in
// RTC memory; at boot the reset reason and that breadcrumb are appended to /.sys/boots.json (last 10).
// GET /sys.json: uptime, heap now / lowest, largest free block, reset reason, boot log.
namespace diag {

void begin();                  // call right after the file system is mounted
// One breadcrumb per task kind, so a crash in one is not blamed on another.
enum Slot { UI = 0, WEB = 1, FETCH = 2 };
void crumb(Slot slot, const char *what);  // cheap: copies into RTC memory
// After a background fetch: logs it (last 8, in /sys.json "kept") when the heap is >8 KB lower than
// before it, i.e. the fetch left memory allocated.
void fetchDone(uint32_t heapBefore);
String json();

}  // namespace diag
