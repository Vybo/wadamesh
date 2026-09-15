// SPDX-License-Identifier: GPL-3.0-or-later
#include "TDeckProDisplay.h"

#include <Arduino.h>
#include <cstring>

TDeckProDisplay::BusyHook TDeckProDisplay::_busy_hook = nullptr;

// HARDWARE v1.0 pin swap. Applied at FILE scope on purpose: the CSE_CST328
// member below captures the reset pin in the constructor's initialiser list, and
// that library does its own hardware reset with it -- an override placed inside
// resetTouch() would fix our reset and leave the library's pointing at the wrong
// pin, which is worse than not overriding at all because it half-works.
#if defined(TDECK_PRO_HW_V10)
  #undef  PIN_TOUCH_RST
  #define PIN_TOUCH_RST 45
#endif
// Touch reset differs by hardware revision; both are tried at runtime.
#ifndef TDECK_PRO_TOUCH_RST_V11
  #define TDECK_PRO_TOUCH_RST_V11 38
#endif
#ifndef TDECK_PRO_TOUCH_RST_V10
  #define TDECK_PRO_TOUCH_RST_V10 45
#endif
#ifndef PIN_BOARD_1V8_EN
  #define PIN_BOARD_1V8_EN 38        // v1.0 only; v1.1 uses this pin as touch reset
#endif


TDeckProDisplay::TDeckProDisplay()
    : DisplayDriver(WIDTH, HEIGHT),
      _canvas(nullptr),
      _epd(Panel(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BUSY)),
      // Reset pin -1: the library must NOT own it. Which GPIO is the touch reset
      // differs between hardware revisions (45 on v1.0, 38 on v1.1) and is
      // decided at runtime below, so the reset is driven by touchTryWiring().
      // CSE_CST328::begin() skips its own reset when this is -1.
      _cst328(WIDTH, HEIGHT, &Wire, -1, PIN_TOUCH_INT) {}

bool TDeckProDisplay::begin() {
  const uint8_t selects[] = { PIN_TFT_CS, P_LORA_NSS, PIN_SD_CS };
  for (uint8_t pin : selects) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  _canvas.setPsram(true);
  _canvas.setColorDepth(16);
  if (!_canvas.createSprite(WIDTH, HEIGHT)) return false;
  _canvas.fillScreen(0xFFFF);

  _mono = (uint8_t*)ps_malloc(MONO_BYTES);
  _sent = (uint8_t*)ps_malloc(MONO_BYTES);
  if (!_mono) _mono = (uint8_t*)malloc(MONO_BYTES);
  if (!_sent) _sent = (uint8_t*)malloc(MONO_BYTES);
  if (!_mono || !_sent) return false;
  memset(_mono, 0xFF, MONO_BYTES);
  memset(_sent, 0xFF, MONO_BYTES);

  SPI.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
  _epd.init(115200, true, 2, false, SPI,
            SPISettings(2000000, MSBFIRST, SPI_MODE0));
  _epd.setRotation(0);
  _epd.epd2.setBusyCallback(&TDeckProDisplay::busyCallback);

  // NOTE: the frontlight LEDC is deliberately NOT attached here. On hardware
  // v1.0 GPIO45 is the TOUCH RESET, not a frontlight -- attaching a channel and
  // writing 0 (which is what this used to do) holds the touch controller in
  // reset for the entire session. It never ACKs, touch reports missing, and the
  // keyboard on the same bus keeps working, which is exactly how this presented.
  // The channel is attached after the revision is known, below.
  Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL, 400000);

  // Which GPIO is the touch reset depends on the board revision, and the two
  // revisions effectively swap two pins:
  //
  //   v1.0 : touch reset = GPIO45, GPIO38 = 1.8 V rail enable, no frontlight
  //   v1.1 : touch reset = GPIO38, GPIO45 = frontlight
  //
  // Getting it wrong is silent and total -- the controller simply stays in reset
  // and never answers -- so rather than trust a build flag, try the compiled
  // preference first and fall back to the other. One binary then works on both,
  // and the flag only chooses which is attempted first.
  // v1.0 is tried FIRST, and the asymmetry of getting it wrong is why:
  //
  //   wrong order on a v1.0 board -> the v1.1 attempt pulses GPIO38 LOW, and on
  //     v1.0 that pin is the 1.8 V rail enable. Cutting the rail under the touch
  //     controller (and whatever else it feeds) to probe a pin is a real risk.
  //   wrong order on a v1.1 board -> the v1.0 attempt drives GPIO38 high (which
  //     on v1.1 just holds reset inactive) and pulses GPIO45 (the frontlight).
  //     Harmless; the cost is that a v1.1 unit can answer on the first attempt
  //     and be recorded as v1.0, losing only its frontlight.
  //
  // A cosmetic loss on one revision beats power-cycling a rail on the other, so
  // the safe order is v1.0 first. TDECK_PRO_HW_V11 forces the other preference
  // for a board where the frontlight matters more than the margin.
#if defined(TDECK_PRO_HW_V11)
  const bool prefer_v10 = false;
#else
  const bool prefer_v10 = true;
#endif
  _touch_ready = touchTryWiring(prefer_v10);
  _hw_rev_v10 = prefer_v10;
  if (!_touch_ready) {
    _touch_ready = touchTryWiring(!prefer_v10);
    if (_touch_ready) _hw_rev_v10 = !prefer_v10;
  }
  if (_touch_ready) {
    _touch_is_cst3530 = false;
    _cst328.setRotation(0);
  } else {
    // Neither wiring produced a CST328. Only now is the CST3530 worth trying;
    // it is the rarer part, and its probe shares 0xCACA with the CST328's own
    // debug-mode chip ID, so it must never be consulted first.
    _touch_is_cst3530 = probeCst3530();
    if (_touch_is_cst3530) {
      pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
      _touch_ready = initCst3530();
    }
  }

  // Frontlight, now that the revision is known. v1.0 has none, and GPIO45 there
  // is the touch reset we just used -- attaching a PWM channel to it would undo
  // the init that just succeeded.
  if (!_hw_rev_v10) {
    ledcSetup(TDECK_PRO_FRONTLIGHT_CHANNEL, 12000, 8);
    ledcAttachPin(PIN_TFT_LEDA_CTL, TDECK_PRO_FRONTLIGHT_CHANNEL);
    writeBrightness(0);
  }

  // I2C scan, deliberately LAST -- after the touch controller is initialised.
  //
  // It used to sit between resetTouch() and CSE_CST328::begin(), and that is
  // very likely what was breaking touch init: 112 zero-length address writes
  // aimed at a freshly-reset controller, immediately before the chip-ID read
  // that begin() depends on.
  //
  // It is also NOT authoritative for the touch chip. A bare
  // beginTransmission/endTransmission pair with no payload is a zero-length
  // write, which the CST328 does not ACK -- so 0x1A is absent from this list
  // even on a unit whose touch demonstrably works. Read it for what ELSE is on
  // the bus (keyboard 0x34, IMU 0x28, gauge 0x55, charger 0x6B). The
  // "touch ... try=N" field is the authoritative answer for the controller
  // itself; the absence of 0x1A here means nothing.
  {
    size_t n = 0;
    _i2c_scan[0] = '\0';
    for (uint8_t addr = 0x08; addr < 0x78 && n + 4 < sizeof _i2c_scan; ++addr) {
      if (addr == (uint8_t)PIN_TOUCH_ADDR) continue;   // never poke the touch chip
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0)
        n += (size_t)snprintf(_i2c_scan + n, sizeof _i2c_scan - n, n ? " %02X" : "%02X", addr);
    }
    if (n == 0) snprintf(_i2c_scan, sizeof _i2c_scan, "(bus empty)");
  }

  _is_on = true;
  Serial.printf("[BOOT] T-Deck Pro e-paper ready touch=%s\n",
                _touch_ready ? (_touch_is_cst3530 ? "CST3530" : "CST328") : "missing");
  return true;
}

void TDeckProDisplay::turnOn() {
  _sleeping = false;
  _is_on = true;
  requestRefresh(true);
  writeBrightness(_brightness);
}

void TDeckProDisplay::turnOff() {
  writeBrightness(0);
  _epd.powerOff();
  _sleeping = true;
  _is_on = false;
}

void TDeckProDisplay::clear() {
  _canvas.fillScreen(0xFFFF);
  memset(_mono, 0xFF, MONO_BYTES);
  requestRefresh(true);
}

void TDeckProDisplay::startFrame(ColorVal background) {
  _canvas.fillScreen(background);
  _canvas.setTextColor(_color);
}

void TDeckProDisplay::setTextSize(int size) { _canvas.setTextSize(size); }
void TDeckProDisplay::setColor(ColorVal color) { _color = color; _canvas.setTextColor(color); }
void TDeckProDisplay::setCursor(int x, int y) { _canvas.setCursor(x, y); }
void TDeckProDisplay::print(const char* text) { _canvas.print(text ? text : ""); }
void TDeckProDisplay::fillRect(int x, int y, int w, int h) { _canvas.fillRect(x, y, w, h, _color); }
void TDeckProDisplay::drawRect(int x, int y, int w, int h) { _canvas.drawRect(x, y, w, h, _color); }
void TDeckProDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  _canvas.drawXBitmap(x, y, bits, w, h, _color);
}
uint16_t TDeckProDisplay::getTextWidth(const char* text) { return (uint16_t)_canvas.textWidth(text ? text : ""); }

void TDeckProDisplay::endFrame() {
  canvasToMono();
  requestRefresh(true);
  serviceRefresh(true);
}

bool TDeckProDisplay::isDark(uint16_t color) {
  const uint16_t red = (uint16_t)(((color >> 11) & 0x1F) * 255u / 31u);
  const uint16_t green = (uint16_t)(((color >> 5) & 0x3F) * 255u / 63u);
  const uint16_t blue = (uint16_t)((color & 0x1F) * 255u / 31u);
  return ((red * 54u + green * 183u + blue * 19u) >> 8) < 144u;
}

void TDeckProDisplay::prepareMapTileRGB565(uint16_t* pixels, int width, int height) {
  if (!pixels || width <= 0 || height <= 0) return;

  // A fixed Bayer screen preserves mid-tone map regions without carrying
  // diffusion error across independently decoded tiles. Map tiles are 256 px,
  // a multiple of eight, so the pattern also remains continuous at tile edges.
  static constexpr uint8_t bayer[8][8] = {
    { 0, 48, 12, 60,  3, 51, 15, 63 },
    {32, 16, 44, 28, 35, 19, 47, 31 },
    { 8, 56,  4, 52, 11, 59,  7, 55 },
    {40, 24, 36, 20, 43, 27, 39, 23 },
    { 2, 50, 14, 62,  1, 49, 13, 61 },
    {34, 18, 46, 30, 33, 17, 45, 29 },
    {10, 58,  6, 54,  9, 57,  5, 53 },
    {42, 26, 38, 22, 41, 25, 37, 21 },
  };
  constexpr int black_point = 72;
  constexpr int white_point = 246;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint16_t color = pixels[(size_t)y * width + x];
      const int red = (int)(((color >> 11) & 0x1F) * 255u / 31u);
      const int green = (int)(((color >> 5) & 0x3F) * 255u / 63u);
      const int blue = (int)((color & 0x1F) * 255u / 31u);
      int maximum = red > green ? red : green;
      if (blue > maximum) maximum = blue;
      int minimum = red < green ? red : green;
      if (blue < minimum) minimum = blue;

      // Hue differences carry map meaning even when their luminance is close.
      // Darkening saturated fills slightly keeps water, parks, and major roads
      // distinguishable from neutral land after conversion to one bit.
      int tone = ((red * 54 + green * 183 + blue * 19) >> 8) -
                 (maximum - minimum) / 5;
      bool black;
      if (tone <= black_point) {
        black = true;
      } else if (tone >= white_point) {
        black = false;
      } else {
        const int white_level =
            ((tone - black_point) * 64 + (white_point - black_point) / 2) /
            (white_point - black_point);
        black = bayer[y & 7][x & 7] >= white_level;
      }
      pixels[(size_t)y * width + x] = black ? 0x0000u : 0xFFFFu;
    }
  }
}

void TDeckProDisplay::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_mono || !pixels || w <= 0 || h <= 0) return;
  if (x >= 0 && y >= 0 && x + w <= WIDTH && y + h <= HEIGHT)
    _canvas.pushImage(x, y, w, h, const_cast<uint16_t*>(pixels));
  for (int row = 0; row < h; ++row) {
    const int py = y + row;
    if (py < 0 || py >= HEIGHT) continue;
    for (int col = 0; col < w; ++col) {
      const int px = x + col;
      if (px < 0 || px >= WIDTH) continue;
      uint8_t& destination = _mono[(size_t)py * (WIDTH / 8) + ((size_t)px >> 3)];
      const uint8_t mask = (uint8_t)(0x80u >> (px & 7));
      if (isDark(pixels[(size_t)row * w + col])) destination &= (uint8_t)~mask;
      else destination |= mask;
    }
  }
  requestRefresh(false);
}

void TDeckProDisplay::setDisplayRotation(uint8_t) { setLogicalSize(WIDTH, HEIGHT); }

void TDeckProDisplay::setBrightness(uint8_t brightness) {
  _brightness = brightness;
  if (_sent_valid && !_sleeping) writeBrightness(brightness);
}

void TDeckProDisplay::writeBrightness(uint8_t brightness) {
  // v1.0 has no frontlight, and its GPIO45 is the touch reset -- driving a PWM
  // channel there would reset the touch controller on every brightness change.
  if (_hw_rev_v10) { (void)brightness; return; }
  ledcWrite(TDECK_PRO_FRONTLIGHT_CHANNEL, brightness);
}

void TDeckProDisplay::setRefreshPolicy(uint16_t min_interval_ms, uint8_t full_every_n) {
  _min_interval_ms = min_interval_ms;
  _full_every_n = full_every_n;
}

void TDeckProDisplay::requestRefresh(bool full) {
  _refresh_pending = true;
  _full_refresh_pending = _full_refresh_pending || full;
}

void TDeckProDisplay::serviceRefresh(bool force) {
  if (!_refresh_pending || !_mono) return;
  // _sleeping must not veto an EXPLICITLY forced commit. This check used to sit
  // ahead of `force` and swallowed it, which is why the once-a-minute lock-screen
  // redraw never appeared: every idle path on this board routes through
  // touchScreenBacklight(false) -> display.turnOff() -> _sleeping = true, so the
  // periodic block was firing on schedule into a function that could not commit.
  //
  // Committing while "asleep" is well-defined on e-paper: the panel holds its
  // image with the controller unpowered, and serviceRefresh brackets its own
  // transfer with powerOff() below.
  if (_sleeping && !force) return;
  const uint32_t now = millis();
  if (!force && _last_refresh_ms && now - _last_refresh_ms < _min_interval_ms) return;

  const bool full = _full_refresh_pending ||
                    (_full_every_n != 0 && _partial_refreshes >= _full_every_n);
  if (!full && _sent_valid && memcmp(_mono, _sent, MONO_BYTES) == 0) {
    _refresh_pending = false;
    return;
  }

  digitalWrite(P_LORA_NSS, HIGH);
  digitalWrite(PIN_SD_CS, HIGH);
  digitalWrite(PIN_TFT_CS, HIGH);
  if (full) _epd.setFullWindow();
  else _epd.setPartialWindow(0, 0, WIDTH, HEIGHT);
  _epd.firstPage();
  do {
    _epd.drawInvertedBitmap(0, 0, _mono, WIDTH, HEIGHT, GxEPD_BLACK);
  } while (_epd.nextPage());
  _epd.powerOff();

  memcpy(_sent, _mono, MONO_BYTES);
  _sent_valid = true;
  // Not from a commit taken while asleep -- an idle redraw must not relight the
  // frontlight. Mirrors the existing guard in setBrightness().
  if (!_sleeping) writeBrightness(_brightness);
  _last_refresh_ms = millis();
  _refresh_pending = false;
  _full_refresh_pending = false;
  _partial_refreshes = full ? 0 : (uint8_t)(_partial_refreshes + 1);
}

void TDeckProDisplay::busyCallback(const void*) {
  if (_busy_hook) _busy_hook();
  else delay(1);
}

void TDeckProDisplay::canvasToMono() {
  memset(_mono, 0xFF, MONO_BYTES);
  for (int y = 0; y < HEIGHT; ++y) {
    for (int x = 0; x < WIDTH; ++x) {
      if (isDark((uint16_t)_canvas.readPixelValue(x, y)))
        _mono[(size_t)y * (WIDTH / 8) + ((size_t)x >> 3)] &= (uint8_t)~(0x80u >> (x & 7));
    }
  }
}

// Bring the touch controller up assuming one specific board revision, and
// report whether it answered. Safe to call for the wrong revision: the worst
// case is a brief pulse on a pin that is not the reset, and a failed chip-ID
// read. CSE_CST328::begin() leaves `inited` false on failure, so a second call
// with the other wiring is a genuine retry rather than a cached success.
bool TDeckProDisplay::touchTryWiring(bool v10) {
  const int rst_pin = v10 ? TDECK_PRO_TOUCH_RST_V10 : TDECK_PRO_TOUCH_RST_V11;
  if (v10) {
    // The 1.8 V rail feeds the touch controller on this revision; nothing
    // answers until it is up.
    pinMode(PIN_BOARD_1V8_EN, OUTPUT);
    digitalWrite(PIN_BOARD_1V8_EN, HIGH);
    delay(10);
  }
  pinMode(rst_pin, OUTPUT);
  digitalWrite(rst_pin, HIGH); delay(20);
  digitalWrite(rst_pin, LOW);  delay(80);
  digitalWrite(rst_pin, HIGH); delay(120);   // TRON is 200 ms worst case; begin() retries too

  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    _touch_attempts++;
    if (_cst328.begin()) return true;
    delay(20);
  }
  return false;
}

void TDeckProDisplay::resetTouch() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(20);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(80);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(20);
}

bool TDeckProDisplay::probeCst3530() {
  const uint8_t command[] = { 0xD0, 0x03, 0x00, 0x00 };
  uint8_t response[7] = {};
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
    Wire.write(command, sizeof(command));
    if (Wire.endTransmission() == 0 &&
        Wire.requestFrom((int)PIN_TOUCH_ADDR, (int)sizeof(response)) == sizeof(response)) {
      Wire.readBytes(response, sizeof(response));
      if (response[2] == 0xCA && response[3] == 0xCA) return true;
    }
    const uint8_t wake[] = { 0xD0, 0x00, 0x04, 0x00 };
    Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
    Wire.write(wake, sizeof(wake));
    (void)Wire.endTransmission();
    delay(50);
  }
  return false;
}

bool TDeckProDisplay::writeCst3530Command(uint32_t command) {
  const uint8_t bytes[] = {
    (uint8_t)(command >> 24), (uint8_t)(command >> 16),
    (uint8_t)(command >> 8), (uint8_t)command,
  };
  Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
  Wire.write(bytes, sizeof(bytes));
  return Wire.endTransmission() == 0;
}

bool TDeckProDisplay::initCst3530() {
  bool ok = writeCst3530Command(0xD0000400);
  delay(20);
  ok = writeCst3530Command(0xD0000400) && ok;
  delay(20);
  ok = writeCst3530Command(0xD0000000) && ok;
  ok = writeCst3530Command(0xD0000C00) && ok;
  ok = writeCst3530Command(0xD0000100) && ok;
  return ok;
}

bool TDeckProDisplay::readCst3530(int16_t& x, int16_t& y) {
  const uint8_t read_command[] = { 0xD0, 0x07, 0x00, 0x00 };
  const uint8_t clear_command[] = { 0xD0, 0x00, 0x02, 0xAB };
  uint8_t response[50] = {};

  if (digitalRead(PIN_TOUCH_INT) != LOW) return false;
  Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
  Wire.write(read_command, sizeof(read_command));
  if (Wire.endTransmission() != 0 || Wire.requestFrom((int)PIN_TOUCH_ADDR, 9) != 9) return false;
  size_t received = Wire.readBytes(response, 9);
  const uint8_t fingers = response[3] & 0x0F;
  const uint8_t keys = (response[3] >> 4) & 0x0F;
  const uint8_t total = (uint8_t)(fingers + keys);
  if (total > 1) {
    size_t extra = (size_t)(total - 1) * 5u;
    if (extra > sizeof(response) - received) extra = sizeof(response) - received;
    if (Wire.requestFrom((int)PIN_TOUCH_ADDR, (int)extra) == extra)
      received += Wire.readBytes(response + received, extra);
  }
  Wire.beginTransmission((uint8_t)PIN_TOUCH_ADDR);
  Wire.write(clear_command, sizeof(clear_command));
  (void)Wire.endTransmission();

  if (fingers == 0 || (response[8] >> 4) == 0) return false;
  const size_t index = (size_t)keys * 5u;
  if (index + 7 >= received) return false;
  x = (int16_t)(response[index + 4] | ((uint16_t)(response[index + 7] & 0x0F) << 8));
  y = (int16_t)(response[index + 5] | ((uint16_t)(response[index + 7] & 0xF0) << 4));
  return true;
}

bool TDeckProDisplay::getTouchPoint(uint16_t& x, uint16_t& y) {
  if (!_touch_ready) return false;
  int16_t raw_x = 0, raw_y = 0;
  if (_touch_is_cst3530) {
    if (!readCst3530(raw_x, raw_y)) return false;
  } else {
    if (_cst328.getTouches() == 0) return false;
    const CSE_TouchPoint point = _cst328.getPoint(0);
    raw_x = point.x;
    raw_y = point.y;
  }
  // Stamped BEFORE the range test on purpose: a swapped or out-of-range axis
  // has to be visible rather than silently dropped.
  _dbg_raw_x = raw_x;
  _dbg_raw_y = raw_y;
  if (raw_x < 0 || raw_y < 0 || raw_x >= WIDTH || raw_y >= HEIGHT) return false;
  x = (uint16_t)raw_x;
  y = (uint16_t)raw_y;
  return true;
}