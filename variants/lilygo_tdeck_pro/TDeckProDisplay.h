// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/ui/DisplayDriver.h>

#define ENABLE_GxEPD2_GFX 0
#include <GxEPD2_BW.h>
#include <CSE_CST328.h>
#include <LovyanGFX.hpp>
#include <SPI.h>
#include <Wire.h>

class TDeckProDisplay : public DisplayDriver {
public:
  using BusyHook = void (*)();

  TDeckProDisplay();
  bool begin();

  bool isOn() override { return _is_on; }
  bool isEink() override { return true; }
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal background = UIColor::window_bkg) override;
  void setTextSize(int size) override;
  void setColor(ColorVal color) override;
  void setCursor(int x, int y) override;
  void print(const char* text) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* text) override;
  void endFrame() override;

  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);
  static void prepareMapTileRGB565(uint16_t* pixels, int width, int height);
  void setDisplayRotation(uint8_t rotation);
  void setBrightness(uint8_t brightness);
  bool getTouchPoint(uint16_t& x, uint16_t& y);

  // What the touch probe actually decided, and the last raw sample it saw.
  // Serial is unusable on this firmware (the companion protocol owns the UART),
  // so this is the only way to find out which controller a given unit has --
  // and the two are driven by completely different protocols, so picking wrong
  // is silent and total.
  bool touchReady() const { return _touch_ready; }
  const char* touchKindName() const {
    return _touch_ready ? (_touch_is_cst3530 ? "CST3530" : "CST328") : "none";
  }
  void lastTouchRaw(int16_t& x, int16_t& y) const { x = _dbg_raw_x; y = _dbg_raw_y; }

  // Compact list of every address that ACKed on the shared I2C bus at boot,
  // e.g. "1A 34 55 6B". The single most useful fact when touch is silent: it
  // separates "the controller is not on the bus" (held in reset, unpowered, or
  // wrong pins) from "it is there but the driver is talking to it wrongly".
  // The keyboard at 0x34 doubles as a control -- if it shows and 0x1A does not,
  // the bus itself is fine.
  const char* i2cScan() const { return _i2c_scan; }
  uint8_t touchAttempts() const { return _touch_attempts; }
  bool hwRevV10() const { return _hw_rev_v10; }
  void serviceRefresh(bool force = false);
  void setBusyHook(BusyHook hook) { _busy_hook = hook; }

  // Refresh policy, driven from the persisted prefs rather than compiled in.
  //
  // Both numbers are a trade-off a user legitimately owns, not a tuning
  // constant: the minimum interval trades staleness against how often the panel
  // visibly changes, and the de-ghost count trades accumulated ghosting against
  // how often the screen flashes to black. Someone reading static text wants the
  // calmest possible screen; someone watching a chart wants freshness.
  //
  // full_every_n == 0 legitimately means "never de-ghost automatically" and is
  // not clamped up.
  void setRefreshPolicy(uint16_t min_interval_ms, uint8_t full_every_n);

  // Public entry point for a deliberate de-ghost (settings action, page change,
  // wake). requestRefresh() itself stays private: callers outside this class
  // have no business scheduling an ordinary partial update -- that is what
  // writePixelsRGB565 already does implicitly.
  void requestFullRefresh() { requestRefresh(true); }

private:
  using Panel = GxEPD2_310_GDEQ031T10;
  // Full-height page buffer: with _pages == 1 the do/while in serviceRefresh
  // walks the 240x320 source bitmap ONCE per commit instead of 8 pages x 2
  // fast-partial phases (~1.23M inner iterations). Costs 9600 bytes of internal
  // DRAM instead of 1200, and matters most while locked, where the CPU has been
  // dropped to 80 MHz for the idle redraw.
  using Eink = GxEPD2_BW<Panel, 320>;

  static void busyCallback(const void*);
  static bool isDark(uint16_t color);
  void resetTouch();
  bool probeCst3530();
  bool writeCst3530Command(uint32_t command);
  bool initCst3530();
  bool readCst3530(int16_t& x, int16_t& y);
  void canvasToMono();
  void requestRefresh(bool full = false);
  void writeBrightness(uint8_t brightness);

  static BusyHook _busy_hook;
  static constexpr int WIDTH = 240;
  static constexpr int HEIGHT = 320;
  static constexpr size_t MONO_BYTES = (size_t)WIDTH * HEIGHT / 8;

  lgfx::LGFX_Sprite _canvas;
  Eink _epd;
  CSE_CST328 _cst328;
  uint8_t* _mono = nullptr;
  uint8_t* _sent = nullptr;
  uint16_t _color = 0x0000;
  uint32_t _last_refresh_ms = 0;
  uint8_t _partial_refreshes = 0;
  // Defaults match the constants these replaced, so behaviour is unchanged until
  // a stored preference says otherwise.
  uint16_t _min_interval_ms = 250;
  uint8_t _full_every_n = 9;
  uint8_t _brightness = 255;
  bool _refresh_pending = false;
  bool _full_refresh_pending = true;
  bool _sleeping = false;
  bool _touch_ready = false;
  bool _touch_is_cst3530 = false;
  int16_t _dbg_raw_x = -1;   // last raw sample, BEFORE the bounds test
  int16_t _dbg_raw_y = -1;
  char _i2c_scan[40] = "(not run)";
  uint8_t _touch_attempts = 0;   // how many begin() tries it took (or failed after)
  bool _hw_rev_v10 = false;      // which wiring actually brought touch up
  bool touchTryWiring(bool v10);
  bool _sent_valid = false;
  bool _is_on = false;
};