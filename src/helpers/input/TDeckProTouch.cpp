// SPDX-License-Identifier: GPL-3.0-or-later
//
// LilyGo T-Deck Pro capacitive touch — CST328 at I2C 0x1A, 240x320 portrait.
//
// Implements the shared touch API declared in HeltecV4CapTouch.h (the
// device-neutral one every touch board links its own copy of), so UITask's
// lvglTouchRead() consumes this exactly as it does the T-Deck's GT911.
//
// TWO CONTROLLERS, ONE ADDRESS
// ----------------------------
// Units ship with either a CST328 or a CST3530 and BOTH answer at 0x1A with
// incompatible register protocols. The CST3530 additionally self-sleeps, so it
// cannot be polled blind -- it has to be read on its INT edge.
//
// DETECTION ORDER MATTERS, and getting it backwards is silent. The first cut of
// this file probed for the CST3530 by writing 0xD0 0x03 and testing for a 0xCACA
// reply -- but 0xCACA is the CST328's OWN debug-mode chip ID (see
// CSE_CST328: enter 0xD101, read the info register, check (id >> 16) == 0xCACA).
// So a perfectly healthy CST328 identified as a CST3530 and was then driven with
// the wrong protocol, and touch did nothing at all.
//
// The CST328 is therefore tried FIRST, by its documented mode sequence, and the
// CST3530 is the fallback. No chip-ID read is used: a successful mode-register
// write is the presence test, which avoids depending on an info-register address
// the published constants do not actually name.
//
// The CST328 register map follows CIRCUITSTATE's CSE_CST328 (the driver the
// camillia-mt T-Deck Pro port uses): 16-bit register addresses, finger 1 block
// at 0xD000, state in the low nibble of byte 0, and the 12-bit X/Y split across
// bytes 1-3. The CST3530 half is camillia-mt's reverse-engineering and remains
// UNVERIFIED.
//
// heltecV4CapTouchDebug() reports which controller was selected AND the last raw
// sample, and is surfaced in the on-screen diag panel -- the only diagnostic
// channel this build has (Serial belongs to the companion protocol).
#if defined(HAS_TDECK_PRO) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>
#include <helpers/ui/MomentaryButton.h>   // BUTTON_EVENT_*
#include "HeltecV4CapTouch.h"

#ifndef PIN_TOUCH_SDA
  #define PIN_TOUCH_SDA 13
#endif
#ifndef PIN_TOUCH_SCL
  #define PIN_TOUCH_SCL 14
#endif
#ifndef PIN_TOUCH_RST
  #define PIN_TOUCH_RST 38     // hardware v1.1 (v1.0 wires this to 45)
#endif
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT 12
#endif
#ifndef TDECK_PRO_TOUCH_ADDR
  #define TDECK_PRO_TOUCH_ADDR 0x1A
#endif
#ifndef TDECK_PRO_SWIPE_MIN
  #define TDECK_PRO_SWIPE_MIN 40
#endif

static const int SCR_W = 240;      // native portrait; no MADCTL on this panel
static const int SCR_H = 320;

static bool     s_init_ok   = false;
static bool     s_is_3530   = false;
static char     s_diag[96]  = "touch: (not run)";

static volatile uint16_t s_dbg_rawx = 0, s_dbg_rawy = 0;
static volatile uint8_t  s_dbg_state = 0;   // last byte 0 of the finger-1 block
static volatile bool     s_irq_fired = false;

static bool     s_down = false;
static unsigned long s_down_at = 0;
static uint16_t s_start_x = 0, s_start_y = 0;
static bool     s_live = false;
static uint16_t s_live_x = 0, s_live_y = 0;
static bool     s_tap_pending = false;
static uint16_t s_tap_x = 0, s_tap_y = 0;
static bool     s_swiping_now = false;
static bool     s_swipe_pending = false;
static int8_t   s_swipe_x = 0, s_swipe_y = 0;
static uint8_t  s_ui_rotation = 0;
static uint8_t  s_point_rotation = 0;
static bool     s_slow_poll = false;
static TaskHandle_t s_poll_task = nullptr;

static void IRAM_ATTR touchIsr() { s_irq_fired = true; }

// ---------------------------------------------------------------------------
// CST3530 — command-response protocol, INT-driven (it sleeps between touches)
// ---------------------------------------------------------------------------

static bool cst3530Cmd(uint32_t cmd) {
  const uint8_t b[] = { (uint8_t)(cmd >> 24), (uint8_t)(cmd >> 16),
                        (uint8_t)(cmd >> 8),  (uint8_t)cmd };
  Wire.beginTransmission((uint8_t)TDECK_PRO_TOUCH_ADDR);
  Wire.write(b, sizeof b);
  return Wire.endTransmission() == 0;
}

static bool cst3530Probe() {
  const uint8_t query[] = { 0xD0, 0x03, 0x00, 0x00 };
  uint8_t resp[7] = {};
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    Wire.beginTransmission((uint8_t)TDECK_PRO_TOUCH_ADDR);
    Wire.write(query, sizeof query);
    if (Wire.endTransmission() == 0
        && Wire.requestFrom((int)TDECK_PRO_TOUCH_ADDR, (int)sizeof resp) == (int)sizeof resp) {
      Wire.readBytes(resp, sizeof resp);
      if (resp[2] == 0xCA && resp[3] == 0xCA) return true;   // positive identity
    }
    cst3530Cmd(0xD0000400);   // wake and retry — it may have been asleep
    delay(50);
  }
  return false;
}

static bool cst3530Init() {
  bool ok = cst3530Cmd(0xD0000400); delay(20);
  ok = cst3530Cmd(0xD0000400) && ok; delay(20);
  ok = cst3530Cmd(0xD0000000) && ok;
  ok = cst3530Cmd(0xD0000C00) && ok;
  ok = cst3530Cmd(0xD0000100) && ok;
  return ok;
}

static bool cst3530Read(uint16_t* x, uint16_t* y) {
  const uint8_t read_cmd[]  = { 0xD0, 0x07, 0x00, 0x00 };
  const uint8_t clear_cmd[] = { 0xD0, 0x00, 0x02, 0xAB };
  uint8_t resp[50] = {};

  Wire.beginTransmission((uint8_t)TDECK_PRO_TOUCH_ADDR);
  Wire.write(read_cmd, sizeof read_cmd);
  if (Wire.endTransmission() != 0 || Wire.requestFrom((int)TDECK_PRO_TOUCH_ADDR, 9) != 9) return false;
  size_t got = Wire.readBytes(resp, 9);

  const uint8_t fingers = resp[3] & 0x0F;
  const uint8_t keys    = (resp[3] >> 4) & 0x0F;
  const uint8_t total   = (uint8_t)(fingers + keys);
  if (total > 1) {
    size_t extra = (size_t)(total - 1) * 5u;
    if (extra > sizeof resp - got) extra = sizeof resp - got;
    if ((size_t)Wire.requestFrom((int)TDECK_PRO_TOUCH_ADDR, (int)extra) == extra)
      got += Wire.readBytes(resp + got, extra);
  }

  Wire.beginTransmission((uint8_t)TDECK_PRO_TOUCH_ADDR);
  Wire.write(clear_cmd, sizeof clear_cmd);
  (void)Wire.endTransmission();

  if (fingers == 0 || (resp[8] >> 4) == 0) return false;
  const size_t i = (size_t)keys * 5u;
  if (i + 7u >= got) return false;
  *x = (uint16_t)(resp[i + 4] | ((uint16_t)(resp[i + 7] & 0x0F) << 8));
  *y = (uint16_t)(resp[i + 5] | ((uint16_t)(resp[i + 7] & 0xF0) << 4));
  return true;
}

// ---------------------------------------------------------------------------
// CST328 — plain big-endian register map, pollable
// ---------------------------------------------------------------------------

static bool cst328ReadReg(uint16_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission((uint8_t)TDECK_PRO_TOUCH_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((int)TDECK_PRO_TOUCH_ADDR, (int)len) != (int)len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

// Bare 16-bit register address with no payload -- this chip's mode registers are
// selected by addressing them, not by writing a value to them.
static bool cst328Write16(uint16_t reg) {
  Wire.beginTransmission((uint8_t)TDECK_PRO_TOUCH_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  return Wire.endTransmission(true) == 0;
}

// Debug-info mode, then straight back to normal. The round trip is what arms the
// controller to report; without it the data registers read as zeros forever.
static bool cst328Init() {
  if (!cst328Write16(0xD101)) return false;   // REG_MODE_DEBUG_INFO
  delay(10);
  if (!cst328Write16(0xD109)) return false;   // REG_MODE_NORMAL
  delay(10);
  return true;
}

// Finger 1 block at 0xD000. Byte 0 low nibble is the touch state (6 == down),
// and X/Y are 12-bit values split across bytes 1-3. Note there is a two-byte gap
// at 0xD005/0xD006 before finger 2, which is why a finger count cannot simply be
// read from 0xD005 -- the first cut of this file did exactly that and treated the
// gap as a count.
static bool cst328Read(uint16_t* x, uint16_t* y) {
  uint8_t d[5] = {};
  if (!cst328ReadReg(0xD000, d, sizeof d)) return false;
  s_dbg_state = d[0];
  if ((d[0] & 0x0F) != 6) return false;       // no finger down
  *x = (uint16_t)(((uint16_t)d[1] << 4) | ((d[3] >> 4) & 0x0F));
  *y = (uint16_t)(((uint16_t)d[2] << 4) |  (d[3] & 0x0F));
  return true;
}

// ---------------------------------------------------------------------------
// Gesture state machine — same shape as TDeckTouch's, so the UI behaves
// identically across boards.
// ---------------------------------------------------------------------------

static void touchPoll() {
  uint16_t rx = 0, ry = 0;
  bool pressed;

  if (s_is_3530) {
    // Cannot be polled blind: it sleeps. Only read when the line says so.
    if (!s_irq_fired && digitalRead(PIN_TOUCH_INT) != LOW) pressed = false;
    else { s_irq_fired = false; pressed = cst3530Read(&rx, &ry); }
  } else {
    pressed = cst328Read(&rx, &ry);
  }

  if (pressed) {
    s_dbg_rawx = rx;                 // stamped BEFORE the range test on purpose:
    s_dbg_rawy = ry;                 // a swapped axis must be visible, not dropped
    if (rx >= SCR_W || ry >= SCR_H) pressed = false;
  }

  if (pressed) {
    s_live = true; s_live_x = rx; s_live_y = ry;
    if (!s_down) {
      s_down = true; s_down_at = millis();
      s_start_x = rx; s_start_y = ry;
      s_swiping_now = false;
    } else {
      const int dx = (int)rx - (int)s_start_x;
      const int dy = (int)ry - (int)s_start_y;
      if (!s_swiping_now && (abs(dx) >= TDECK_PRO_SWIPE_MIN || abs(dy) >= TDECK_PRO_SWIPE_MIN))
        s_swiping_now = true;
    }
    return;
  }

  s_live = false;
  if (!s_down) return;

  // Finger lifted — classify.
  const int dx = (int)s_live_x - (int)s_start_x;
  const int dy = (int)s_live_y - (int)s_start_y;
  s_down = false;

  if (abs(dx) >= TDECK_PRO_SWIPE_MIN || abs(dy) >= TDECK_PRO_SWIPE_MIN) {
    int8_t sx = 0, sy = 0;
    if (abs(dx) > abs(dy)) sx = dx > 0 ? 1 : -1;
    else                   sy = dy > 0 ? 1 : -1;
    // Rotate the gesture into what the user sees. The point itself stays in raw
    // panel space (LVGL transforms that); only the direction is rotated.
    switch (s_ui_rotation) {
      case 1: { const int8_t t = sx; sx = (int8_t)-sy; sy = t; break; }
      case 2: sx = (int8_t)-sx; sy = (int8_t)-sy; break;
      case 3: { const int8_t t = sx; sx = sy; sy = (int8_t)-t; break; }
      default: break;
    }
    s_swipe_x = sx; s_swipe_y = sy; s_swipe_pending = true;
  } else {
    s_tap_x = s_start_x; s_tap_y = s_start_y; s_tap_pending = true;
  }
  s_swiping_now = false;
}

static void pollTaskFn(void* arg) {
  const uint32_t period = (uint32_t)(uintptr_t)arg;
  for (;;) {
    touchPoll();
    vTaskDelay(pdMS_TO_TICKS(s_slow_poll ? 100 : period));
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool heltecV4CapTouchBegin() {
  if (s_init_ok) return true;

  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000);

  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(20);
  digitalWrite(PIN_TOUCH_RST, LOW);  delay(80);
  digitalWrite(PIN_TOUCH_RST, HIGH); delay(20);

  // CST328 first -- see the header note on why the old 0xCACA probe mis-sorted
  // a healthy CST328 into the CST3530 path.
  s_is_3530 = false;
  s_init_ok = cst328Init();
  if (!s_init_ok) {
    s_is_3530 = cst3530Probe();
    if (s_is_3530) {
      pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
      s_init_ok = cst3530Init();
      if (s_init_ok) {
        s_irq_fired = false;
        attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), touchIsr, FALLING);
      }
    }
  }
  return s_init_ok;
}

int heltecV4CapTouchCheck() {
  if (!s_init_ok) return BUTTON_EVENT_NONE;
  if (!s_poll_task) touchPoll();     // inline poll until the async task is up
  if (s_tap_pending) return BUTTON_EVENT_CLICK;
  return BUTTON_EVENT_NONE;
}

bool heltecV4CapTouchPopTap(uint16_t* x, uint16_t* y) {
  if (!s_tap_pending) return false;
  s_tap_pending = false;
  if (x) *x = s_tap_x;
  if (y) *y = s_tap_y;
  return true;
}

bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y) {
  if (!s_live) return false;
  if (x) *x = s_live_x;
  if (y) *y = s_live_y;
  return true;
}

bool heltecV4CapTouchPopSwipe(int8_t* x_dir, int8_t* y_dir) {
  if (!s_swipe_pending) return false;
  s_swipe_pending = false;
  if (x_dir) *x_dir = s_swipe_x;
  if (y_dir) *y_dir = s_swipe_y;
  return true;
}

bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms) {
  if (s_poll_task || !s_init_ok) return false;
  if (period_ms < 4)   period_ms = 4;
  if (period_ms > 100) period_ms = 100;
  // Core 0, same as the T-Deck's: core 1 runs loop(), and on this board loop()
  // can be inside a 1 s e-paper BUSY wait. Touch must stay sampled through it
  // or taps queued during a refresh are lost.
  return xTaskCreatePinnedToCore(pollTaskFn, "proTouch", 3072,
                                 (void*)(uintptr_t)period_ms, 1, &s_poll_task, 0) == pdPASS;
}

bool heltecV4CapTouchIsAsyncPolling() { return s_poll_task != nullptr; }
bool heltecV4CapTouchIsSwiping()      { return s_swiping_now; }
void heltecV4CapTouchSetRotation(uint8_t r)      { s_ui_rotation = r; }
void heltecV4CapTouchSetPointRotation(uint8_t r) { s_point_rotation = r; }
void heltecV4CapTouchSetSlowPoll(bool slow)      { s_slow_poll = slow; }
// Regenerated on every call rather than stamped once at boot: the live diag
// overlay polls this at 4 Hz, and on a board with no readable Serial the raw
// sample IS the instrument. `st` is byte 0 of the finger-1 block (low nibble 6
// means finger down); raw x,y are reported BEFORE the on-screen bounds check, so
// a swapped or out-of-range axis is visible rather than silently dropped.
const char* heltecV4CapTouchDebug() {
  snprintf(s_diag, sizeof s_diag, "%s@0x%02X %s st=%02X raw=%u,%u",
           s_is_3530 ? "CST3530" : "CST328", (unsigned)TDECK_PRO_TOUCH_ADDR,
           s_init_ok ? "ok" : "MISSING", (unsigned)s_dbg_state,
           (unsigned)s_dbg_rawx, (unsigned)s_dbg_rawy);
  return s_diag;
}

void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry) {
  if (rx) *rx = s_dbg_rawx;
  if (ry) *ry = s_dbg_rawy;
}

#endif
