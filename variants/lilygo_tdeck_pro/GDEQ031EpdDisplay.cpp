// SPDX-License-Identifier: GPL-3.0-or-later
#include "GDEQ031EpdDisplay.h"

#if defined(HAS_TDECK_PRO) && defined(ESP32)

#include <Arduino.h>
#include <string.h>
#include <GxEPD2_BW.h>

#ifndef PIN_EPD_CS
  #define PIN_EPD_CS   34
#endif
#ifndef PIN_EPD_DC
  #define PIN_EPD_DC   35
#endif
#ifndef PIN_EPD_RST
  #define PIN_EPD_RST  16   // hardware v1.1. v1.0 has no reset line: pass -1 and
                            // GxEPD2 degrades hibernate() to powerOff().
#endif
#ifndef PIN_EPD_BUSY
  #define PIN_EPD_BUSY 37
#endif
#ifndef PIN_EPD_FRONTLIGHT
  #define PIN_EPD_FRONTLIGHT 45
#endif
#ifndef EPD_FRONTLIGHT_PWM_CH
  #define EPD_FRONTLIGHT_PWM_CH 2   // 0/1 are taken by the keyboard backlight path
#endif

// The panel is on the bus shared with the SX1262 and the microSD, so every
// other CS is parked high before an update. There is no bus mutex anywhere in
// this firmware — the invariant is "everything runs sequentially on the loop
// task" — so this is belt-and-braces against a half-finished transaction, not
// arbitration.
#ifndef P_LORA_NSS
  #define P_LORA_NSS 3
#endif
#ifndef PIN_SD_CS
  #define PIN_SD_CS 48
#endif

static constexpr size_t kMonoBytes  = (size_t)GDEQ031EpdDisplay::PANEL_WIDTH
                                    * (size_t)GDEQ031EpdDisplay::PANEL_HEIGHT / 8u;
static constexpr size_t kRowStride  = (size_t)GDEQ031EpdDisplay::PANEL_WIDTH / 8u;

static void (*s_busy_hook)() = nullptr;

// 4x4 ordered Bayer, scaled to 0..255. Ordered rather than error-diffused on
// purpose: Floyd-Steinberg is content-dependent, so a one-pixel scroll changes
// the dither phase of the whole frame and every commit becomes a full-frame
// diff. An ordered matrix is a pure function of (x, y), so identical content
// packs to identical bytes and the no-change skip in serviceRefresh() keeps
// working.
static const uint8_t kBayer4[4][4] = {
  {  15, 143,  47, 175 },
  { 207,  79, 239, 111 },
  {  63, 191,  31, 159 },
  { 255, 127, 223,  95 },
};

// Rec.601 luma from RGB565, without leaving integer maths. The 5/6/5 channels
// are expanded to 8 bits first so a saturated channel really reaches 255.
static inline uint8_t lumaFromRgb565(uint16_t px) {
  const uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
  const uint8_t g = (uint8_t)(((px >>  5) & 0x3F) * 255 / 63);
  const uint8_t b = (uint8_t)(( px        & 0x1F) * 255 / 31);
  return (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
}

// The GxEPD2 instantiation, kept out of the header (see the note there).
// Page height == panel height, i.e. one un-paged full framebuffer: 240*320/8 =
// 9,600 bytes.
struct GDEQ031Impl {
  GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> epd;
  GDEQ031Impl()
    : epd(GxEPD2_310_GDEQ031T10(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY)) { }
};

GDEQ031EpdDisplay::GDEQ031EpdDisplay()
  : DisplayDriver(PANEL_WIDTH, PANEL_HEIGHT),
    _impl(nullptr),
    _mono(nullptr), _sent(nullptr), _have_sent(false), _lvgl_active(false),
    _is_on(false), _dirty(false), _force_full(true), _partial_count(0),
    _brightness_pct(0), _min_interval_ms(400), _full_every_n(5),
    _last_commit_at(0), _last_refresh_ms(0), _last_band_at(0), _last_was_full(false),
    _text_color(0xFFFF)
{ }

GDEQ031EpdDisplay::~GDEQ031EpdDisplay() {
  delete _impl;
  free(_mono);
  free(_sent);
}

void GDEQ031EpdDisplay::setBusyHook(void (*hook)()) { s_busy_hook = hook; }

void GDEQ031EpdDisplay::busyCallback(const void* p) {
  (void)p;
  if (s_busy_hook) s_busy_hook();
  else delay(1);
}

bool GDEQ031EpdDisplay::begin() {
  // Park the neighbours on the shared bus before the panel's first transaction.
  pinMode(P_LORA_NSS, OUTPUT); digitalWrite(P_LORA_NSS, HIGH);
  pinMode(PIN_SD_CS,  OUTPUT); digitalWrite(PIN_SD_CS,  HIGH);
  pinMode(PIN_EPD_CS, OUTPUT); digitalWrite(PIN_EPD_CS, HIGH);

  if (!_impl) _impl = new GDEQ031Impl();
  if (!_impl) return false;

  _mono = (uint8_t*)malloc(kMonoBytes);
  _sent = (uint8_t*)malloc(kMonoBytes);
  if (!_mono || !_sent) {
    free(_mono); free(_sent);
    _mono = _sent = nullptr;
    return false;                    // caller leaves the panel dark; never deref below
  }
  fillShadow(true);
  memset(_sent, 0xFF, kMonoBytes);
  _have_sent = false;

  // The SPI bus is already begun for the radio (variants/.../target.cpp). Hand
  // GxEPD2 the same instance rather than letting it call SPI.begin() a second
  // time with its own pin set.
  _impl->epd.init(115200, true, 2, false);
  _impl->epd.epd2.setBusyCallback(&GDEQ031EpdDisplay::busyCallback);
  _impl->epd.setRotation(0);               // native portrait; no MADCTL on this panel
  _impl->epd.setTextWrap(false);
  _impl->epd.setTextColor(GxEPD_BLACK);
  _impl->epd.setTextSize(1);

#if PIN_EPD_FRONTLIGHT >= 0
  ledcSetup(EPD_FRONTLIGHT_PWM_CH, 12000, 8);
  ledcAttachPin(PIN_EPD_FRONTLIGHT, EPD_FRONTLIGHT_PWM_CH);
  ledcWrite(EPD_FRONTLIGHT_PWM_CH, 0);
#endif

  _is_on = true;
  _force_full = true;                // first paint after boot is always a full
  _dirty = true;
  return true;
}

uint32_t GDEQ031EpdDisplay::msSinceLastBand() const {
  if (_last_band_at == 0) return UINT32_MAX;
  return (uint32_t)(millis() - _last_band_at);
}

void GDEQ031EpdDisplay::fillShadow(bool white) {
  if (_mono) memset(_mono, white ? 0xFF : 0x00, kMonoBytes);
}

// ---------------------------------------------------------------------------
// LVGL path
// ---------------------------------------------------------------------------

void GDEQ031EpdDisplay::packBandToShadow(int x, int y, int w, int h, const uint16_t* pixels) {
  for (int row = 0; row < h; ++row) {
    const int py = y + row;
    if (py < 0 || py >= PANEL_HEIGHT) continue;
    const uint16_t* src = pixels + (size_t)row * (size_t)w;
    uint8_t* dst_row = _mono + (size_t)py * kRowStride;
    const uint8_t* bayer_row = kBayer4[py & 3];

    for (int col = 0; col < w; ++col) {
      const int px = x + col;
      if (px < 0 || px >= PANEL_WIDTH) continue;
      // DARK UI pixel -> black ink. This is the identity mapping, and it is
      // only correct because the UI runs kMonoPalette (a LIGHT theme) on this
      // board -- see applyThemeMode() in UITask.cpp, gated on CAP_MONO.
      //
      // The palette and this line are a matched pair. If the UI ever renders
      // the dark night palette here again, this must invert or the panel floods
      // with ink: a 0x000000 background would become solid black.
      const bool ink = lumaFromRgb565(src[col]) <= bayer_row[px & 3];
      uint8_t& byte = dst_row[px >> 3];
      const uint8_t bit = (uint8_t)(0x80u >> (px & 7));
      if (ink) byte &= (uint8_t)~bit;   // clear = ink
      else     byte |= bit;             // set   = paper
    }
  }
}

void GDEQ031EpdDisplay::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_mono || !pixels || w <= 0 || h <= 0) return;
  packBandToShadow(x, y, w, h, pixels);
  _lvgl_active = true;
  _dirty = true;
  _last_band_at = millis();
  // Deliberately no refresh here. lvglFlush is per-band; committing here would
  // mean ~14 panel updates per full repaint. UITask's loop calls
  // serviceRefresh() once the bands have stopped arriving.
}

// ---------------------------------------------------------------------------
// DisplayDriver text path (boot splash, remote placeholder) — draws into
// GxEPD2's own Adafruit_GFX framebuffer, not into _mono.
// ---------------------------------------------------------------------------

void GDEQ031EpdDisplay::startFrame(ColorVal bkg) {
  if (!_impl) return;
  // Whatever the caller nominally asks for, paper is white here. main.cpp's
  // splash passes an explicit black because an LCD's window_bkg is black; on
  // e-paper that would be a near-solid-ink screen, slow and ghost-prone.
  (void)bkg;
  _impl->epd.setFullWindow();
  _impl->epd.firstPage();
  _impl->epd.fillScreen(GxEPD_WHITE);
  _lvgl_active = false;
  _dirty = true;
}

void GDEQ031EpdDisplay::setTextSize(int sz)  { if (_impl) _impl->epd.setTextSize(sz < 1 ? 1 : sz); }
void GDEQ031EpdDisplay::setCursor(int x, int y) { if (_impl) _impl->epd.setCursor(x, y); }
void GDEQ031EpdDisplay::print(const char* str)  { if (_impl && str) _impl->epd.print(str); }

void GDEQ031EpdDisplay::setColor(ColorVal c) {
  _text_color = (uint16_t)c;
  if (!_impl) return;
  // One bit of ink, thresholded at mid-luma to match the LVGL path's
  // dark-pixel-is-ink rule (both paths must agree, or the boot splash renders
  // inverted relative to the UI that replaces it).
  _impl->epd.setTextColor(lumaFromRgb565(_text_color) < 128 ? GxEPD_BLACK : GxEPD_WHITE);
}

void GDEQ031EpdDisplay::fillRect(int x, int y, int w, int h) {
  if (!_impl) return;
  _impl->epd.fillRect(x, y, w, h, lumaFromRgb565(_text_color) < 128 ? GxEPD_BLACK : GxEPD_WHITE);
}

void GDEQ031EpdDisplay::drawRect(int x, int y, int w, int h) {
  if (!_impl) return;
  _impl->epd.drawRect(x, y, w, h, lumaFromRgb565(_text_color) < 128 ? GxEPD_BLACK : GxEPD_WHITE);
}

void GDEQ031EpdDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  if (!_impl) return;
  _impl->epd.drawXBitmap(x, y, bits, w, h, GxEPD_BLACK);
}

uint16_t GDEQ031EpdDisplay::getTextWidth(const char* str) {
  if (!_impl || !str) return 0;
  int16_t  bx = 0, by = 0;
  uint16_t bw = 0, bh = 0;
  _impl->epd.getTextBounds(str, 0, 0, &bx, &by, &bw, &bh);
  return bw;
}

void GDEQ031EpdDisplay::endFrame() {
  // On an LCD this is a no-op. Here it is the whole point: it is the only
  // signal that a non-LVGL frame is complete and may be shown.
  requestRefresh(false);
}

void GDEQ031EpdDisplay::clear() {
  fillShadow(true);
  _lvgl_active = true;
  _dirty = true;
  _force_full = true;   // a deliberate blank is the right moment to de-ghost
}

// ---------------------------------------------------------------------------
// Power / frontlight
// ---------------------------------------------------------------------------

void GDEQ031EpdDisplay::turnOn() {
  if (_is_on) return;
  _is_on = true;
  _force_full = true;                   // coming back from powerOff, de-ghost
  requestRefresh(true);
  setBrightness(_brightness_pct);
}

void GDEQ031EpdDisplay::turnOff() {
  if (!_is_on || !_impl) return;
#if PIN_EPD_FRONTLIGHT >= 0
  ledcWrite(EPD_FRONTLIGHT_PWM_CH, 0);
#endif
  // NOT a panel reset: e-paper keeps its image with the controller unpowered,
  // which is the whole point of "screen off" on this board. The last frame
  // stays readable.
  _impl->epd.powerOff();
  _is_on = false;
}

void GDEQ031EpdDisplay::setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  _brightness_pct = pct;
#if PIN_EPD_FRONTLIGHT >= 0
  ledcWrite(EPD_FRONTLIGHT_PWM_CH, _is_on ? (uint32_t)pct * 255u / 100u : 0u);
#endif
}

// ---------------------------------------------------------------------------
// Refresh policy
// ---------------------------------------------------------------------------

void GDEQ031EpdDisplay::setRefreshPolicy(uint16_t min_interval_ms, uint8_t full_every_n) {
  _min_interval_ms = min_interval_ms;
  _full_every_n    = full_every_n;
}

void GDEQ031EpdDisplay::requestRefresh(bool full) {
  _dirty = true;
  if (full) _force_full = true;
}

bool GDEQ031EpdDisplay::serviceRefresh(bool force) {
  if (!_dirty || !_is_on || !_mono || !_impl) return false;

  const uint32_t now = millis();
  if (!force && _last_commit_at != 0
      && (uint32_t)(now - _last_commit_at) < _min_interval_ms) {
    return false;                       // throttled; stays dirty for next tick
  }

  const bool full = _force_full
                 || (_full_every_n != 0 && _partial_count >= _full_every_n);

  if (_lvgl_active) {
    // Nothing changed since the last push: skip the whole 0.7-1.1 s update.
    // This is what keeps an idle device from flashing — it is the backstop for
    // every redraw the UI issues without actually changing a pixel.
    if (!full && _have_sent && memcmp(_mono, _sent, kMonoBytes) == 0) {
      _dirty = false;
      return false;
    }
  }

  digitalWrite(P_LORA_NSS, HIGH);
  digitalWrite(PIN_SD_CS,  HIGH);

  const uint32_t started = millis();
  if (full) _impl->epd.setFullWindow();
  else      _impl->epd.setPartialWindow(0, 0, PANEL_WIDTH, PANEL_HEIGHT);

  if (_lvgl_active) {
    // drawInvertedBitmap: a SET bit renders as the background (paper), a CLEAR
    // bit as GxEPD_BLACK. That matches the convention _mono is packed in.
    _impl->epd.firstPage();
    do {
      _impl->epd.fillScreen(GxEPD_WHITE);
      _impl->epd.drawInvertedBitmap(0, 0, _mono, PANEL_WIDTH, PANEL_HEIGHT, GxEPD_BLACK);
    } while (_impl->epd.nextPage());
    memcpy(_sent, _mono, kMonoBytes);
    _have_sent = true;
  } else {
    // The text path already drew into GxEPD2's framebuffer between
    // startFrame() and endFrame(); just page it out.
    while (_impl->epd.nextPage()) { }
    _have_sent = false;                 // _sent no longer describes the glass
  }
  _impl->epd.powerOff();

  _last_refresh_ms = millis() - started;
  _last_commit_at  = millis();
  _last_was_full   = full;
  _dirty      = false;
  _force_full = false;
  _partial_count = full ? 0 : (uint8_t)(_partial_count + 1);
  return true;
}

#endif
