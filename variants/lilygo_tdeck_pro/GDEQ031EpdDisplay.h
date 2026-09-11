// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LilyGo T-Deck Pro — Good Display GDEQ031T10 (UC8253) 240x320 1-bpp e-paper,
// driven through GxEPD2 and presented to the rest of the firmware as an
// ordinary DisplayDriver.
//
// WHY THIS IS SHAPED DIFFERENTLY FROM EVERY OTHER DISPLAY CLASS HERE
// ------------------------------------------------------------------
// On an LCD, writePixelsRGB565() pushes a band down SPI and returns in well
// under a millisecond, and endFrame() is a no-op. An e-paper panel inverts that
// relationship completely, for two reasons that both have to be designed around
// rather than worked around:
//
//  1. A refresh COSTS 0.7-1.1 s and BLOCKS. GxEPD2 spins on the BUSY line inside
//     _waitWhileBusy() for the whole update. There is no LVGL task in this
//     firmware — main.cpp's loop() runs ui_task.loop() (which is what calls
//     lv_timer_handler() -> lvglFlush -> here) and the_mesh.loop() strictly
//     sequentially on the same Arduino loopTask. So any wait taken inside a
//     flush delays mesh processing 1:1. The firmware's own stallLog() treats
//     200 ms as pathology; a refresh is 5x that.
//
//  2. lvglFlush() is a PER-BAND callback. The draw buffer is 240 x
//     LV_DRAW_BUF_LINES, so a full-screen repaint arrives as ~14 separate
//     writePixelsRGB565() calls. Refreshing per call would mean 14 panel
//     updates per frame — around 15 seconds for one screen.
//
// So writePixelsRGB565() here NEVER touches the panel. It converts the band to
// 1 bpp and composites it into a persistent full-frame shadow, and that is all.
// The panel update happens later, once, from epdServiceRefresh(), which the UI
// loop calls at its own cadence (see UITask.cpp, next to webMirrorTick()).
// This is the same accumulate-then-commit split the web mirror already uses,
// and the same one the vendored core's GxEPDDisplay::endFrame() uses.
//
// TWO DRAWING PATHS, ONE PANEL
// ----------------------------
// Pixels reach this class two different ways and they cannot share a buffer:
//
//  * The LVGL path (writePixelsRGB565) composites into _mono.
//  * The DisplayDriver text path (startFrame/print/fillRect/drawRect/drawXbm)
//    is used by the boot splash in main.cpp and by drawRemotePlaceholder() in
//    UITask.cpp, both of which run OUTSIDE LVGL. Those draw straight into
//    GxEPD2's own Adafruit_GFX framebuffer.
//
// _lvgl_active records which path last wrote, and epdServiceRefresh() commits
// from that one. The takeover is not sticky in either direction: the next
// writePixelsRGB565() hands control back to the LVGL path on its own.
//
// COLOUR
// ------
// The UI is a dark theme (COLOR_BG is 0x000000, text is near-white), so a naive
// threshold would flood the panel with ink. Luma maps to ink the other way
// round here: a BRIGHT UI pixel becomes BLACK INK on white paper. Edges are
// ordered-dithered through a 4x4 Bayer matrix rather than hard-thresholded,
// because LVGL renders 4-bpp antialiased glyphs at LV_COLOR_DEPTH 16 and a hard
// cut turns every glyph edge into a ragged stair.
//
// Bit convention in _mono matches what GxEPD2's drawInvertedBitmap() wants:
// a SET bit is white paper, a CLEAR bit is black ink.

#if defined(HAS_TDECK_PRO) && defined(ESP32)

#include <helpers/ui/DisplayDriver.h>
#include <stdint.h>

// GxEPD2 is deliberately NOT included here. The vendored core's ESP32Board.cpp
// does `#include <target.h>`, and this board's target.h reaches this header —
// so every translation unit in the firmware, including ones inside the MeshCore
// library, would need GxEPD2 on its include path. PlatformIO's LDF does not
// propagate it there (the core's library.json knows nothing about GxEPD2), and
// neither chain+ nor deep+ fixes it.
//
// So the panel object lives behind an opaque impl defined in the .cpp. That is
// the correct shape regardless of the build system: nothing outside this class
// has any business seeing the GxEPD2 template instantiation.
struct GDEQ031Impl;

class GDEQ031EpdDisplay : public DisplayDriver {
public:
  static const int PANEL_WIDTH  = 240;
  static const int PANEL_HEIGHT = 320;

  GDEQ031EpdDisplay();
  ~GDEQ031EpdDisplay();

  bool begin();

  // ---- DisplayDriver ----
  bool isOn() override { return _is_on; }
  bool isEink() override { return true; }
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override;

  // ---- Extras called directly on the concrete type (mirrors ST7789LCDDisplay) ----

  /** LVGL flush target. Converts the RGB565 band to 1 bpp and composites it
   *  into the shadow. Does NOT touch the panel — see the file header. */
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);

  /** No-op: this panel has no MADCTL. The UI is native 240x320 portrait and
   *  device_caps.h sets CAP_ROTATABLE 0. Present so the shared UITask.cpp
   *  rotation call sites compile unchanged. */
  void setDisplayRotation(uint8_t r) { (void)r; }

  /** Panel frontlight (GPIO 45) — pct 0..100. Separate from the keyboard
   *  backlight on GPIO 42, which is NOT a display control. */
  void setBrightness(uint8_t pct);
  uint8_t getBrightness() const { return _brightness_pct; }

  /** Ask for a panel update. `full` forces a de-ghosting full refresh instead
   *  of letting the every-N policy decide. Cheap and idempotent — the actual
   *  update happens in epdServiceRefresh(). */
  void requestRefresh(bool full = false);

  /** Commit the pending frame to the panel if policy allows. Call from the UI
   *  loop, NEVER from inside a flush. Returns true if the panel was updated.
   *  `force` bypasses the minimum-interval throttle (not the no-change skip). */
  bool serviceRefresh(bool force = false);

  /** Refresh policy, driven from prefs (see touchPrefsGetEpd* in
   *  TouchPrefsStore). full_every_n == 0 disables automatic full refreshes. */
  void setRefreshPolicy(uint16_t min_interval_ms, uint8_t full_every_n);

  /** Installed by UITask so the ~1 s BUSY wait can pump the things that cannot
   *  wait it out (the mesh loop, the keyboard FIFO). Null until then, in which
   *  case GxEPD2's own 1 ms sleep is used. */
  static void setBusyHook(void (*hook)());

  bool     lastRefreshWasFull() const { return _last_was_full; }
  uint32_t lastRefreshDurationMs() const { return _last_refresh_ms; }

private:
  static void busyCallback(const void* p);

  void      packBandToShadow(int x, int y, int w, int h, const uint16_t* pixels);
  void      fillShadow(bool white);

  GDEQ031Impl* _impl;   // owns the GxEPD2_BW<GxEPD2_310_GDEQ031T10, 320> instance
  uint8_t*   _mono;        // committed shadow: set bit = paper, clear = ink
  uint8_t*   _sent;        // last frame actually pushed, for the no-change skip
  bool       _have_sent;
  bool       _lvgl_active; // which of the two drawing paths owns the next commit
  bool       _is_on;
  bool       _dirty;
  bool       _force_full;
  uint8_t    _partial_count;
  uint8_t    _brightness_pct;
  uint16_t   _min_interval_ms;
  uint8_t    _full_every_n;
  uint32_t   _last_commit_at;
  uint32_t   _last_refresh_ms;
  bool       _last_was_full;
  uint16_t   _text_color;  // RGB565 as handed to setColor(); only luma is used
};

#endif
