# LilyGo T-Deck Pro — port notes

Bring-up scaffold for the T-Deck Pro (e-paper). Follows the shape of
`variants/tdisplay_p4/TDISPLAY_P4_PORT.md` and `variants/thinknode_m9/M9_PORT.md`.

**Status: builds, never run on hardware.** Nothing below has been validated on a
device. Treat every pin and every timing as "sourced, not confirmed".

```
pio run -e LilyGo_TDeckPro_companion_radio_touch     # SUCCESS, RAM 31.8%, Flash 78.5%
```

## Hardware

ESP32-S3FN16R8, 16 MB flash / 8 MB PSRAM. Panel is a Good Display **GDEQ031T10**,
3.1", **240x320 native portrait**, 1 bpp, controller **UC8253**, driven by GxEPD2's
`GxEPD2_310_GDEQ031T10`. Touch is **CST328 @ 0x1A**, but some units ship a
**CST3530** at the same address with an incompatible protocol. Keyboard is a
**TCA8418 @ 0x34** 4x10 matrix — the T-LoRa Pager's controller, *not* the
T-Deck/T-Deck Plus ESP32-C3 keyboard at 0x55. No trackball.

Pins here are **hardware v1.1**. v1.0 differs: no EPD reset line (`PIN_EPD_RST`
must be `-1`, and GxEPD2 then degrades `hibernate()` to `powerOff()`), touch reset
on 45 instead of 38, GPS enable on 15 instead of 39. Sources: LilyGo's
`T-Deck-Pro/utilities.h`, Meshtastic's `t-deck-pro-v1_1` variant, and the
`oumike/camillia-mt` HAL.

Refresh timings, from GxEPD2's driver constants and Good Display's datasheet page:
full 1100 ms (GxEPD2) / 3 s (GD), partial 700 ms / 0.4 s. **GxEPD2's numbers are
BUSY-wait ceilings, not measurements.** Good Display recommends a full de-ghost
after roughly every 5 partials.

### The one unresolved go/no-go

`boards/t-deck-pro.json` sets `memory_type: qio_qspi` (the T-Deck's says
`qio_opi`). If this part is actually **octal** PSRAM, GPIO 33-37 are claimed by
the PSRAM controller — and that is exactly MOSI 33, EPD_CS 34, EPD_DC 35, SCK 36
and EPD_BUSY 37, i.e. the whole display plus the shared bus. Both LilyGo's and
Meshtastic's board files say quad and Meshtastic's build works, so the odds are
good, but LilyGo's JSON is demonstrably a mislabelled T-Watch S3 copy. **The
failure mode is quiet** — PSRAM allocation falls back to `malloc` and starves
DRAM — not a boot crash, so confirm it deliberately rather than assuming a
successful boot proves it.

## The display class

`GDEQ031EpdDisplay.{h,cpp}` here, not in `src/helpers/ui/` — matching the P4,
Heltec, RAK and Tanmatsu classes. Two things about it are not like any other
display class in this tree, and both are forced by the panel:

**The refresh is not in the flush.** `writePixelsRGB565()` dithers the band to
1 bpp into a persistent 9,600-byte shadow and returns. It never touches the
panel. Two independent reasons it cannot:

1. A refresh blocks 0.7-1.1 s inside GxEPD2's `_waitWhileBusy()`. There is no
   LVGL task — `main.cpp`'s `loop()` runs `ui_task.loop()` (which is what reaches
   `lvglFlush`) and `the_mesh.loop()` sequentially on one Arduino loopTask, so a
   wait taken in a flush delays the mesh 1:1. The firmware's own `stallLog()`
   calls 200 ms pathological.
2. `lvglFlush` is a **per-band** callback — `240 x LV_DRAW_BUF_LINES` — so a full
   repaint arrives as ~14 calls. Refreshing per call would be ~14 panel updates
   per frame.

`serviceRefresh()` does the commit and must be called from the UI loop. **It is
not wired up yet** — see TODO 1.

**Colour is inverted on purpose.** The UI is a dark theme (`COLOR_BG` is
`0x000000`), so a bright UI pixel becomes *black ink on white paper*. Edges go
through a 4x4 ordered Bayer matrix rather than a hard threshold, because LVGL
renders 4-bpp antialiased glyphs at `LV_COLOR_DEPTH 16`. Ordered, not
Floyd-Steinberg: an ordered matrix is a pure function of (x, y), so unchanged
content packs to identical bytes and the no-change skip in `serviceRefresh()`
keeps working. Error diffusion would re-dither the whole frame on a one-pixel
scroll and make every commit a full-frame diff.

Bit convention in the shadow: **set bit = paper, clear bit = ink** (what
GxEPD2's `drawInvertedBitmap()` expects).

## Build-system traps hit during bring-up

Three of these cost real time and none of them names its own cause.

**1. `target.h` is scanned by the vendored core, and the scan aborts silently.**
The core's `ESP32Board.cpp` does `#include <target.h>`, so PlatformIO's LDF walks
this file in the *core library's* include scope to decide what MeshCore depends
on. That scope does not contain this variant directory. When the scan hits an
include it cannot resolve there, it **stops and drops every include after it** —
and the error surfaces somewhere else entirely. The observed symptom was the core
failing with `MicroNMEA.h: No such file or directory`, because
`helpers/sensors/MicroNMEALocationProvider.h` sat below the display include and
was therefore never scanned, so MicroNMEA never joined the core's dependency set.

Consequences, all load-bearing:
- `MicroNMEALocationProvider.h` is included from `target.cpp`, not `target.h`
  (nothing in the header needed it — only `sensors` is extern'd, not `gps`).
- `GDEQ031EpdDisplay.h` is **pimpl'd** so it does not pull `GxEPD2_BW.h` into
  every TU that includes `target.h`. The core cannot resolve GxEPD2 either.
- The display include stays in `target.h` because the core calls
  `display.turnOff()` at `ESP32Board.cpp:889` and needs the complete type. Its
  *compile* resolves it fine via the global `-I variants/lilygo_tdeck_pro`; only
  the LDF scan cannot.
- **Any new include added to `target.h` goes ABOVE the `#ifdef DISPLAY_CLASS`
  block**, or gets the same treatment.

`lib_ldf_mode = deep+` does not fix this and introduces its own breakage.

**2. Pin the core to `core-v1.17.4`, not `core-v1.16.5`.** CLAUDE.md still says
1.16.5; every env in `platformio.ini` says 1.17.4. Pinning the old tag gets you a
core whose `DisplayDriver` predates the `ColorVal`/`UIColor` palette API, and a
wall of `'ColorVal' does not name a type` from files you never touched. Worse,
that tag's `library.json` version (1.10.0) collides with an unrelated **registry**
package named `MeshCore`, which PlatformIO then installs into `MeshCore/` while
the git one lands in `MeshCore@src-<hash>/` — and the registry copy wins on the
include path.

**3. `UIColorPalette.cpp` must be named in `build_src_filter`.** Other touch envs
get the `UIColor` statics for free because `build_as_lib.py` compiles
`helpers/ui/<DISPLAY_CLASS>.cpp` from the **core** tree and the upstream driver
carries the definitions. This board's display class is in `variants/`, so no core
display `.cpp` is built and nothing defines them. Symptom: `undefined reference
to UIColor::primary_txt` from `ConsoleUI.cpp` at link. It is named as a single
file, not `+<helpers/ui/*.cpp>`, because the palette must be defined exactly once.

## Shared-code changes outside this directory

- `src/ui-touch/device_caps.h` — a `HAS_TDECK_PRO` block placed **above** the
  `HAS_TDECK_GT911` arm, plus new `CAP_EINK` / `CAP_MONO` / `CAP_SLOW_DISPLAY`.
  `HAS_TDECK_GT911` is deliberately **not** defined: none of its 81 gates is
  about touch, and one of them bit-bangs ST7789 `SLPIN`/`SLPOUT` over its own
  HSPI instance, which on a UC8253 writes garbage to the panel.
- `CAP_KEYPAD_NAV` gains `HAS_TDECK_PRO`. Not optional: the `HAS_PAGER_KEYBOARD`
  blocks in `UITask.cpp` call `navOpenChatPanel`/`navGoToMainTab`/`navPushTap`/
  `s_nav_group`, which only exist under that flag. The pager never noticed the
  coupling because it has no touchscreen.
- `src/ui-touch/UITask.cpp` — a `HAS_TDECK_PRO` arm in the display-type and
  display-header ladders, its own `s_kbd_nav`/`s_tb_nav`/`s_nav_keypad_drv`
  block, an `OTA_BIN_NAME` arm **above** the T-Deck's, and `|| defined(HAS_TDECK_PRO)`
  on the 13 shared microSD gates.
- `src/main.cpp` — `|| defined(HAS_TDECK_PRO)` on the 4 shared-bus/SD gates, and
  a gated `#include <GDEQ031EpdDisplay.h>` for the boot splash.
- `src/helpers/input/TDeckTouch.cpp` — `&& !defined(HAS_TDECK_PRO)` added to its
  negative-guard list. That file is the *default* touch driver for any
  `HAS_TOUCH_UI` board, so a board with its own controller must opt out or it
  silently links the GT911 driver.
- `include/lv_conf.h` — `LV_DISP_DEF_REFR_PERIOD` 500 under `HAS_TDECK_PRO`.
  It has to be a board-gated `#if` in that file: every value there is a bare
  `#define` and `lv_conf_internal.h` includes the file *first*, so a per-env
  `-D` is silently redefined and lost. (The `-D LV_INDEV_DEF_SCROLL_THROW=7` in
  every env is a live example — `lv_hal_indev.h` unconditionally puts it back
  to 10.)

## TODO — what is scaffolded but not done

1. **Wire `serviceRefresh()` into the UI loop.** Nothing calls it, so the panel
   never updates. It belongs next to `webMirrorTick()` in `UITask::loop`, with a
   quiescence gate so one repaint's ~14 bands become one commit.
2. **Install the busy hook.** `GDEQ031EpdDisplay::setBusyHook()` exists and is
   unused. Without it the loop task sleeps through the whole 0.7-1.1 s wait;
   camillia-mt hit exactly this and lost keystrokes to a full TCA8418 FIFO. The
   hook must pump `the_mesh.loop()` as well as the keyboard.
3. **Make the refresh policy configurable** (the stated requirement). Append
   trailing fields to `TouchCfg` in `TouchPrefsStore.cpp` and bump
   `TOUCH_CFG_VER` **40 -> 41** (CLAUDE.md's "currently 5" is 35 versions stale).
   Defaults today are hardcoded in the class: 400 ms minimum interval, full
   refresh every 5 partials.
4. **The keymap is wrong.** The env defines `HAS_PAGER_KEYBOARD` to reuse the
   pager's TCA8418 driver, which also reuses the pager's 4x10 keymap. The Pro's
   35-key layout differs, so characters will be wrong. Needs a Pro keymap table.
5. **Battery reports 0.** `TDeckProBoard::getBattMilliVolts()` returns 0 rather
   than reading the T-Deck's GPIO4 divider, because GPIO4 is `BOARD_LORA_RST` on
   this board. Real reading needs the BQ27220 gauge over I2C (0x55).
6. **CST3530 is unverified.** The protocol in `TDeckProTouch.cpp` is transcribed
   from camillia-mt's reverse-engineering. `heltecV4CapTouchDebug()` reports
   which controller was detected; that string reaches the on-screen diag panel,
   which is the only diagnostic channel here (Serial belongs to the companion
   protocol).
7. **Palette is untouched.** The dark theme still has `COLOR_BG 0x000000`, and 8
   of its 13 constants have luma < 128. `COLOR_SENT_BG` (32) and `COLOR_RECV_BG`
   (28) differ by 4/255, so chat sent-vs-received will not survive 1 bpp. The
   inversion in the display class makes the screen *legible*, not *correct*; a
   board-gated light palette is still needed.
8. **Nothing is registered for release.** No entry in `scripts/release.sh` ENVS,
   `scripts/build/gen-flasher-meta.py` BOARDS, `.github/workflows/release.yml`
   (4 sites), `deploy/site/index.html`, `deploy/flasher/index.html`, DEVICES.md
   or README. `OTA_BIN_NAME` is already `wadamesh-tdeck-pro`, so the device will
   look for a binary nobody publishes. This repo has shipped that exact bug twice
   (commits 92b2ed1, cb08116).
9. **Animations, marquees and timer cadences are untouched.** `CAP_SLOW_DISPLAY`
   is defined and gates nothing yet. `UI_REFRESH_MS` is still 250 and
   `updateGlobalStatusBar` still issues ~217 unconditional `lv_label_set_text`
   calls per tick.

## First hardware steps

Do these before trusting anything above:

1. Read the board revision off the silkscreen. Everything here assumes v1.1.
2. I2C scan. Expect 0x1A touch, 0x23 LTR553, 0x28 BHI260AP, 0x34 TCA8418,
   0x55 BQ27220, 0x6B BQ25896.
3. Confirm PSRAM mode and size (the go/no-go above).
4. Time a partial vs a full refresh, and a small window vs a full-screen one. If
   a 48x16 window is no faster than full-screen on this controller, region-limited
   updates buy nothing and the policy should just always push the full frame —
   which is what camillia-mt does.
