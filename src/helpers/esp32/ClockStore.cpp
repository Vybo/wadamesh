// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClockStore.h"

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#if defined(HAS_TDECK_GT911)
  #include <SD.h>
#endif
#include <string.h>

const char* clockRingtoneName(uint8_t id) {
  switch (id) {
    case RING_SILENT: return "Silent";
    case RING_CHIME:  return "Chime";
    case RING_BEEP:   return "Beep";
    case RING_ASCEND: return "Ascend";
    case RING_URGENT: return "Urgent";
    default:          return "Silent";
  }
}

// ---- backend selector (mirrors UITask's battLogFs): SD on T-Deck w/ card,
// internal SPIFFS otherwise (V4, or a T-Deck with no card inserted) ----
static fs::FS& clockFs() {
#if defined(HAS_TDECK_GT911)
  if (SD.cardType() != CARD_NONE) return SD;
#endif
  return SPIFFS;
}
static bool clockOnSd() {
#if defined(HAS_TDECK_GT911)
  return SD.cardType() != CARD_NONE;
#else
  return false;
#endif
}
// name = "world" / "timers" / "alarms"
static void clockPath(const char* name, char* out, int cap) {
  if (clockOnSd()) snprintf(out, cap, "/meshcomod/clock_%s", name);
  else             snprintf(out, cap, "/clock_%s", name);
}

static const uint8_t CLK_MAGIC = 0xC1;

static bool clockLoadList(const char* name, void* out, int recsz,
                          int max_n, int& n, uint8_t want_ver) {
  n = 0;
  char path[48];
  clockPath(name, path, sizeof path);
  fs::FS& fs = clockFs();
  if (!fs.exists(path)) return true;            // no file yet => empty list
  File f = fs.open(path, FILE_READ);
  if (!f) return false;
  uint8_t hdr[3];
  if (f.read(hdr, 3) != 3 || hdr[0] != CLK_MAGIC || hdr[1] != want_ver) {
    f.close();
    return false;                               // unknown/garbage => treat as empty
  }
  int cnt = hdr[2];
  if (cnt > max_n) cnt = max_n;
  int got = 0;
  for (int i = 0; i < cnt; ++i) {
    if (f.read((uint8_t*)out + (size_t)got * recsz, recsz) != recsz) break;
    ++got;
  }
  f.close();
  n = got;
  return true;
}

static bool clockSaveList(const char* name, const void* in, int recsz,
                          int n, uint8_t ver) {
  char path[48];
  clockPath(name, path, sizeof path);
  fs::FS& fs = clockFs();
#if defined(HAS_TDECK_GT911)
  if (clockOnSd()) SD.mkdir("/meshcomod");
#endif
  File f = fs.open(path, FILE_WRITE);           // FILE_WRITE truncates/creates
  if (!f) return false;
  if (n < 0)   n = 0;
  if (n > 255) n = 255;
  uint8_t hdr[3] = { CLK_MAGIC, ver, (uint8_t)n };
  bool ok = (f.write(hdr, 3) == 3);
  if (ok && n > 0)
    ok = (f.write((const uint8_t*)in, (size_t)n * recsz) == (size_t)n * recsz);
  f.close();
  return ok;
}

bool clockLoadWorld(ClockWorldEntry* out, int max_n, int& n) {
  return clockLoadList("world", out, sizeof(ClockWorldEntry), max_n, n, 1);
}
bool clockSaveWorld(const ClockWorldEntry* in, int n) {
  return clockSaveList("world", in, sizeof(ClockWorldEntry), n, 1);
}
bool clockLoadTimers(ClockTimerPreset* out, int max_n, int& n) {
  return clockLoadList("timers", out, sizeof(ClockTimerPreset), max_n, n, 1);
}
bool clockSaveTimers(const ClockTimerPreset* in, int n) {
  return clockSaveList("timers", in, sizeof(ClockTimerPreset), n, 1);
}
bool clockLoadAlarms(ClockAlarm* out, int max_n, int& n) {
  return clockLoadList("alarms", out, sizeof(ClockAlarm), max_n, n, 1);
}
bool clockSaveAlarms(const ClockAlarm* in, int n) {
  return clockSaveList("alarms", in, sizeof(ClockAlarm), n, 1);
}
