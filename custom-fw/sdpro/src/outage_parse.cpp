#include "outage_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace outage {

// One hour's value -> its two half-hour slots.
static void hourSlots(const char *v, uint8_t &a, uint8_t &b) {
  a = b = UNKNOWN;
  if (!v) return;
  if (!strcmp_P(v, PSTR("yes"))) a = b = ON;
  else if (!strcmp_P(v, PSTR("no"))) a = b = OFF;
  else if (!strcmp_P(v, PSTR("first"))) a = OFF, b = ON;
  else if (!strcmp_P(v, PSTR("second"))) a = ON, b = OFF;
  else if (!strcmp_P(v, PSTR("mfirst"))) a = MAYBE, b = ON;
  else if (!strcmp_P(v, PSTR("msecond"))) a = ON, b = MAYBE;
  else if (!strcmp_P(v, PSTR("maybe")) || !strcmp_P(v, PSTR("mno"))) a = b = MAYBE;
}

static void fillDay(JsonObjectConst hours, uint8_t *slots) {
  for (int h = 0; h < 24; h++) {
    char key[4];
    snprintf(key, sizeof key, "%d", h + 1);
    hourSlots(hours[key], slots[2 * h], slots[2 * h + 1]);
  }
}

static const char NO_ERROR[] PROGMEM = "";
const __FlashStringHelper *lastError = FPSTR(NO_ERROR);

void makeFilter(const char *group, JsonDocument &filter) {
  char gkey[16];
  snprintf(gkey, sizeof gkey, "GPV%s", group);
  filter[F("fact")][F("today")] = true;
  filter[F("fact")][F("update")] = true;
  filter[F("fact")][F("data")]["*"][gkey] = true;
}

bool fromDoc(const JsonDocument &doc, const char *group, Schedule &out) {
  char gkey[16];
  snprintf(gkey, sizeof gkey, "GPV%s", group);
  JsonObjectConst fact = doc[F("fact")];
  if (fact.isNull()) return lastError = F("no fact"), false;

  out = Schedule();
  memset(out.day, UNKNOWN, sizeof out.day);
  out.today = (time_t)(fact[F("today")] | 0LL);
  strncpy(out.update, fact[F("update")] | "", sizeof out.update - 1);
  char key[16];
  for (int d = 0; d < 2; d++) {
    snprintf(key, sizeof key, "%lld", (long long)out.today + d * 86400LL);
    JsonObjectConst hours = fact[F("data")][key][gkey];
    if (hours.isNull()) continue;
    fillDay(hours, out.day[d]);
    (d ? out.hasTomorrow : out.hasToday) = true;
  }
  if (!out.today) lastError = F("no today");
  else if (!out.hasToday) lastError = F("no group");
  else lastError = F("");
  return out.today != 0 && out.hasToday;
}

// ---- Poltavaoblenergo ------------------------------------------------------------------

// Genitive month names as poe.pl.ua writes dates, in flash (fixed width; read with strcmp_P).
static const char MONTHS[12][20] PROGMEM = {"січня", "лютого", "березня", "квітня", "травня", "червня",
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

// Slot state from a cell's class: old layout light_1|2|3, new layout gpv-s1|2|3 (on, off, possible).
static int cellState(const char *tag) {
  if (strncmp_P(tag, PSTR("td"), 2)) return -1;
  const char *l = strstr_P(tag, PSTR("light_"));
  int n = l ? 6 : 0;
  if (!l) {
    for (l = strstr_P(tag, PSTR("gpv-s")); l && !(l[5] >= '1' && l[5] <= '3'); l = strstr_P(l + 5, PSTR("gpv-s"))) {}
    n = 5;
  }
  if (!l) return -1;
  return l[n] == '1' ? ON : l[n] == '2' ? OFF : l[n] == '3' ? MAYBE : UNKNOWN;
}

// Tags: <b> may hold the date; queue/subqueue cells (old layout) or one "1.1" cell (new layout);
// slot cells; </tr> ends a row.
void PoeParser::onTag() {
  int st;
  if (!strcmp_P(tag, PSTR("b"))) want = DATE, textLen = 0;
  else if (!strncmp_P(tag, PSTR("td"), 2) && strstr_P(tag, PSTR("\"gpv-q\""))) want = QNAME, textLen = 0;
  else if (strstr_P(tag, PSTR("turnoff-scheduleui-table-subqueue"))) want = SUBQUEUE, textLen = 0;
  else if (strstr_P(tag, PSTR("turnoff-scheduleui-table-queue"))) want = QUEUE, textLen = 0;
  else if ((st = cellState(tag)) >= 0) {
    if (cells < SLOTS) row[cells++] = st;
  } else if (!strncmp_P(tag, PSTR("tr"), 2)) {
    cells = 0;
  } else if (!strcmp_P(tag, PSTR("/tr"))) {
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
        if (!strcmp_P(month, MONTHS[m])) curY = y, curM = m + 1, curD = d;
  } else if (want == QUEUE) {  // "1 черга": a new queue, the first row is subqueue 1
    queue = atoi(text);
    sub = 1;
  } else if (want == QNAME) {  // "1.1"
    queue = atoi(text);
    const char *dot = strchr(text, '.');
    sub = dot ? atoi(dot + 1) : 1;
  } else if (want == SUBQUEUE) {
    sub = atoi(text);
  }
  want = NONE;
  textLen = 0;
}

}  // namespace outage
