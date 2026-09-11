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

Only the quad variant is shipped. If the board turns out to be octal, it is a
one-line change in `boards/t-deck-pro.json`:

```json
"memory_type": "qio_opi"      // was qio_qspi
```

Symptoms that say you need it: PSRAM reported as 0 bytes in the on-screen diag
panel, an LVGL draw-buffer or map-tile allocation failing, or the panel never
producing an image at all (GPIO 33-37 being held by the PSRAM controller takes
MOSI, EPD_CS, EPD_DC, SCK and EPD_BUSY with it).

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

## What is implemented

All nine follow-ups from the first pass are done. None of it has run on hardware.

**Panel commit.** `epdServiceTick()` (`UITask.cpp`, next to `webMirrorTick()`)
commits a settled frame once per UI loop. Quiescence is `msSinceLastBand() >= 40 ms`
rather than `lv_disp_flush_is_last()` — that flag means "last band of THIS
invalidated area" and LVGL flushes several dirty rectangles per cycle, so it would
commit a fraction of a frame and then repeat.

**BUSY pump.** `epdBusyHook()` is installed once the keyboard is up. It drains the
TCA8418 FIFO (ten events, roughly a second of typing) and yields. It deliberately
does NOT call `the_mesh.loop()`: that has no re-entrancy guard, mutates the outbound
queue and the packet pool, and can kick a multi-second SPIFFS rewrite. LoRa RX across
the stall is covered by the existing core-0 drain task (`rlwRxqTask`), which already
defaults ON for every board — turning it off in Radio & Mesh is a materially worse
idea here than elsewhere.

**Refresh policy** is four `TouchPrefsSchema::Config` trailing fields at
**v56 -> v57** (not 40 -> 41; that figure in the first pass was wrong, and CLAUDE.md's
"currently 5" is 51 versions stale). Settings -> Display gets minimum interval,
de-ghost-every-N, full-on-page-change, full-on-wake and a "Refresh screen now" button.
Dropdowns, not sliders: on this panel every drag step of a slider is its own ~0.7 s
commit. The manual refresh is a settings row and NOT the status-bar 3 s hold — that
gesture is already the SD screenshot, and `CAP_SD` is 1 here.

**Palette.** A third `TouchPalette` (`kMonoPalette`) plus a `CAP_MONO` branch in
`applyThemeMode()`. The premise in the first pass was wrong: it is not 13 constants,
it is a 30-field struct plus 42 mutable `COLOR_*` globals, and `applyThemeMode` is the
single selector. Values sit in seven luma bands ~40 apart, because 16 dither levels
cannot carry finer distinctions; structure is carried by borders, not fills.
`s_theme_day` is forced true, which also routes the 67 `themeRole(night, day)` sites
onto palette roles instead of hardcoded dark literals, and picks
`accentClampReadable`'s 105 ceiling. The ten day-literals in `applyThemeMode`'s tail
are overridden — two of them are dark bubbles meant for a light theme and would have
become identical ink slabs.

**The inversion is gone, in all four places.** With a light palette, ink is the dark
pixel — the identity mapping. `packBandToShadow` and the three text-path thresholds
now agree. The palette and those four lines are a matched pair: if the UI ever renders
the night palette here again, they must invert or the panel floods with ink.

**`styleButton`** gets a `CAP_MONO` arm. Its `LV_OPA_10` fill blends to within a
sixteenth of the background, which the Bayer matrix renders as zero ink — all 159
buttons were invisible. On e-paper a button is a solid ink outline, and pressing it
inverts to a solid fill.

**Motion.** `UI_REFRESH_MS` 250 -> 2000; the four `LV_LABEL_LONG_SCROLL_CIRCULAR`
marquees become `LV_LABEL_LONG_DOT` via `TOUCH_LABEL_LONG_OVERFLOW` (a marquee is
`LV_ANIM_REPEAT_INFINITE`, i.e. a permanent refresh generator);
`LV_THEME_DEFAULT_TRANSITION_TIME` 0; the home chart is floored at one point per 5 s;
the 12 unconditional status-bar label writes go through `setLabelIfChanged`. Scroll
momentum is zeroed on the REGISTERED indev — not on the pre-register struct and not
from a theme `apply_cb`, because `lv_hal_indev.c` overwrites the former and
`lv_obj_constructor` re-ORs `SCROLL_MOMENTUM` after `lv_theme_apply` runs.

Worth knowing: none of the cadence work is what stops the screen flashing when nothing
changed. That is the `memcmp` against the last-sent frame in `serviceRefresh()`, which
skips the commit entirely. The cadence work stops the firmware doing the render in the
first place.

**Keymap.** `PagerKeyboardState` gains a `HAS_TDECK_PRO` arm: new tables plus new
positions (Alt 29, R_Shift 30, L_Shift 34, Sym 31, Backspace 10, Space 32, Enter 20).
The positions were the real hazard, not the tables — the pager's `SHIFT_POS` 28 is
`z` on the Pro and its `SPACE_POS` 30 is R_Shift, so reusing them would silently eat
two real keys. Sym feeds the same `LatchedModifier` as Alt. The pager values stay in
the `#else` arm so `test/test_pager_keyboard_state.cpp` keeps passing.

**Battery.** `getBattMilliVolts()` / `getBattStateOfCharge()` read the BQ27220 at
0x55 (`Voltage()` 0x08, `StateOfCharge()` 0x2C), copied from the T-Display P4's
driver — same chip, same address, same shared-bus situation. Reads are throttled to
5 s, sanity-windowed and hold the last good value; a failed read returns false rather
than 0, which is the bug that made the same gauge report a flickering 0% on the P4.
`batteryPercentFromMv` prefers the gauge's coulomb-counted percentage over the voltage
curve on this board.

**Release registration.** `scripts/release.sh` ENVS, `scripts/build/gen-flasher-meta.py`
BOARDS and the `deploy/site/index.html` board card — the same three files both prior
"compiles forever, ships never" fixes touched (92b2ed1, cb08116). Plus DEVICES.md and
README.md.

Four registration surfaces were deliberately LEFT ALONE, each for a stated reason:
- `.github/workflows/release.yml` — it only knows 2 boards and has never learned about
  the M9, RAK, either pager, Attaky or the Wio Tracker. Adding just the Pro would make
  it inconsistent with seven others rather than correct. `scripts/release.sh` is the
  path that actually ships beta, and the workflow has no beta feed at all.
- `deploy/flasher/index.html` — `deploy/README.md:77-81` states no deploy script
  publishes it and to treat `deploy/site/index.html` as the only reachable install page.
- `deploy/gen-meshamerica-catalog.py` — gated to stable promotes (`release.sh:141`),
  and its `name` must match the official MeshCore device name exactly. A beta-only
  board does not belong there yet.
- `platformio.ini` `default_envs` — lists 3 of 10 envs; every board added since the M9
  was left out. `scripts/build-all-targets.sh` parses the env list straight out of
  `platformio.ini`, so the Pro is already picked up by the real build-everything path.

One known collision, outside this repo tree: `.claude/skills/build-wadamesh-bin/build.sh`
hardcodes `ENV_NAME="LilyGo_TDeck_companion_radio_touch"` and writes
`builds/wadamesh-custom-<YYYYMMDDHHMM>.bin` with no board token, so a T-Deck and a
T-Deck Pro build in the same minute overwrite each other. It lives in the main
checkout's `.claude/`, not here.

## Still unverified

- **Every pin, and the whole keymap.** Two independent upstream sources agree, but
  neither was run on this unit.
- **CST3530.** The protocol is transcribed from camillia-mt's reverse-engineering.
  `heltecV4CapTouchDebug()` reports which controller was detected; it reaches the
  on-screen diag panel, which is the only diagnostic channel here.
- **PSRAM mode** — the go/no-go above.
- **Whether a region-limited refresh is faster than a full-screen one** on UC8253.
  Nothing here depends on it (the driver always pushes the whole frame, as camillia-mt
  does), but if it IS faster there is a real win left on the table.
- **Keyboard vs touch I2C contention.** `CAP_TOUCH` is 1, so the core-0 touch task
  polls the same Wire bus at 125 Hz that the keyboard is drained from on the UI loop.
  The pager never had this problem because it has no touchscreen.

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
