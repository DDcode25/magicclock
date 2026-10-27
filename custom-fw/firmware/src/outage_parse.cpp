#include "outage_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace outage {

// One hour's value -> its two half-hour slots.
static void hourSlots(const char *v, uint8_t &a, uint8_t &b) {
  a = b = UNKNOWN;
  if (!v) return;
  if (!strcmp(v, "yes")) a = b = ON;
  else if (!strcmp(v, "no")) a = b = OFF;
  else if (!strcmp(v, "first")) a = OFF, b = ON;
  else if (!strcmp(v, "second")) a = ON, b = OFF;
  else if (!strcmp(v, "mfirst")) a = MAYBE, b = ON;
  else if (!strcmp(v, "msecond")) a = ON, b = MAYBE;
  else if (!strcmp(v, "maybe") || !strcmp(v, "mno")) a = b = MAYBE;
}

static void fillDay(JsonObjectConst hours, uint8_t *slots) {
  for (int h = 0; h < 24; h++) {
    char key[4];
    snprintf(key, sizeof key, "%d", h + 1);
    hourSlots(hours[key], slots[2 * h], slots[2 * h + 1]);
  }
}

const char *lastError = "";

void makeFilter(const char *group, JsonDocument &filter) {
  char gkey[16];
  snprintf(gkey, sizeof gkey, "GPV%s", group);
  filter["fact"]["today"] = true;
  filter["fact"]["update"] = true;
  filter["fact"]["data"]["*"][gkey] = true;
}

bool fromDoc(const JsonDocument &doc, const char *group, Schedule &out) {
  char gkey[16];
  snprintf(gkey, sizeof gkey, "GPV%s", group);
  JsonObjectConst fact = doc["fact"];
  if (fact.isNull()) return lastError = "no fact", false;

  out = Schedule();
  memset(out.day, UNKNOWN, sizeof out.day);
  out.today = (time_t)(fact["today"] | 0LL);
  strncpy(out.update, fact["update"] | "", sizeof out.update - 1);
  char key[16];
  for (int d = 0; d < 2; d++) {
    snprintf(key, sizeof key, "%lld", (long long)out.today + d * 86400LL);
    JsonObjectConst hours = fact["data"][key][gkey];
    if (hours.isNull()) continue;
    fillDay(hours, out.day[d]);
    (d ? out.hasTomorrow : out.hasToday) = true;
  }
  if (!out.today) lastError = "no today";
  else if (!out.hasToday) lastError = "no group";
  else lastError = "";
  return out.today != 0 && out.hasToday;
}

// ---- Poltavaoblenergo ------------------------------------------------------------------

static const char *const MONTHS[] = {"січня", "лютого", "березня", "квітня", "травня", "червня",
                                     "липня", "серпня", "вересня", "жовтня", "листопада", "грудня"};

PoeParser::PoeParser(const char *group) {
  wantQueue = atoi(group);
  const char *dot = strchr(group, '.');
  wantSub = dot ? atoi(dot + 1) : 1;
  memset(found, 0, sizeof found);
}

void PoeParser::feed(char c) {
  if (c == '<') {
    if (textLen) onText();
    inTag = true;
    tagLen = 0;
    return;
  }
  if (inTag) {
    if (c == '>') {
      inTag = false;
      tag[tagLen] = 0;
      onTag();
    } else if (tagLen < (int)sizeof tag - 1) {
      tag[tagLen++] = c;
    }
    return;
  }
  if (want != NONE && textLen < (int)sizeof text - 1) text[textLen++] = c;
}

// Tags: <b> may hold the date; queue/subqueue cells; light_N cells; </tr> ends a row.
void PoeParser::onTag() {
  if (!strcmp(tag, "b")) want = DATE, textLen = 0;
  else if (strstr(tag, "turnoff-scheduleui-table-subqueue")) want = SUBQUEUE, textLen = 0;
  else if (strstr(tag, "turnoff-scheduleui-table-queue")) want = QUEUE, textLen = 0;
  else if (const char *l = strstr(tag, "light_")) {
    if (cells < SLOTS) row[cells++] = l[6] == '1' ? ON : l[6] == '2' ? OFF : l[6] == '3' ? MAYBE : UNKNOWN;
  } else if (!strncmp(tag, "tr", 2)) {
    cells = 0;
  } else if (!strcmp(tag, "/tr")) {
    if (cells == SLOTS && queue == wantQueue && sub == wantSub && curY) {
      for (int i = 0; i < count; i++)
        if (found[i].year == curY && found[i].month == curM && found[i].day == curD) return;
      if (count < 2) {
        PoeDay &d = found[count++];
        d.year = curY, d.month = curM, d.day = curD, d.found = true;
        memcpy(d.slots, row, SLOTS);
      }
    }
    cells = 0;
  }
}

void PoeParser::onText() {
  text[textLen] = 0;
  if (want == DATE) {  // "27 жовтня 2026 року"
    int d = 0, y = 0;
    char month[32] = "";
    if (sscanf(text, "%d %31s %d", &d, month, &y) == 3)
      for (int m = 0; m < 12; m++)
        if (!strcmp(month, MONTHS[m])) curY = y, curM = m + 1, curD = d;
  } else if (want == QUEUE) {  // "1 черга": a new queue, the first row is subqueue 1
    queue = atoi(text);
    sub = 1;
  } else if (want == SUBQUEUE) {
    sub = atoi(text);
  }
  want = NONE;
  textLen = 0;
}

}  // namespace outage
