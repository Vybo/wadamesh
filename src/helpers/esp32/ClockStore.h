// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// Persistent data layer for the touch-UI Clock app: world-clock zones, countdown
// timer presets, and alarms. Board-portable: the list files live on the SD card
// when present (T-Deck, under /meshcomod) and fall back to internal SPIFFS on the
// Heltec V4 (flat top-level path) — mirroring the battery-log FS selector. Each
// list is a whole-file rewrite of fixed-size packed records behind a small
// [magic][ver][count] header. Reorder a record field after ship => bump its ver.

// Ringtone identifiers. The note tables + playback live in UITask.cpp (they need
// the board audio drivers); records persist only the id, names live here.
enum ClockRingtone : uint8_t {
  RING_SILENT = 0,
  RING_CHIME,
  RING_BEEP,
  RING_ASCEND,
  RING_URGENT,
  RING_COUNT
};
const char* clockRingtoneName(uint8_t id);

struct __attribute__((packed)) ClockWorldEntry {
  uint8_t tz_index;     // index into the curated TZ preset table (see TouchPrefsStore)
  int8_t  custom_hr;    // UTC offset hours when tz_index is the Custom slot (reserved; v1 adds presets only)
  char    label[24];    // display label; empty => use the preset's label
};

struct __attribute__((packed)) ClockTimerPreset {
  char     name[24];
  uint32_t dur_sec;     // countdown duration in seconds
  uint8_t  ringtone;    // ClockRingtone played on expiry
};

struct __attribute__((packed)) ClockAlarm {
  uint8_t hour;         // 0..23
  uint8_t minute;       // 0..59
  uint8_t days_mask;    // bit0=Sun .. bit6=Sat; 0 => one-time (auto-disables after firing)
  uint8_t enabled;      // 0/1
  uint8_t ringtone;     // ClockRingtone
  char    label[24];
};

static const int CLOCK_MAX_WORLD  = 12;
static const int CLOCK_MAX_TIMERS = 12;
static const int CLOCK_MAX_ALARMS = 16;

// Load returns true on success (a missing file is success => n=0). Save rewrites
// the whole file. All are no-ops-safe to call repeatedly.
bool clockLoadWorld (ClockWorldEntry* out, int max_n, int& n);
bool clockSaveWorld (const ClockWorldEntry* in, int n);
bool clockLoadTimers(ClockTimerPreset* out, int max_n, int& n);
bool clockSaveTimers(const ClockTimerPreset* in, int n);
bool clockLoadAlarms(ClockAlarm* out, int max_n, int& n);
bool clockSaveAlarms(const ClockAlarm* in, int n);
