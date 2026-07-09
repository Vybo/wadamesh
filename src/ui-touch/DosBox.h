// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Native DOS / IBM PC-XT player built on the Faux86-remake core vendored under
// ui-touch/faux86/ (GPL-2.0-or-later; folds forward into wadamesh's GPL-3).
//
// SCAFFOLD — step 1 of the port. Class shell + build wiring only: it pulls the
// Faux86 8086/V20 core into the T-Deck firmware and exposes the SnakeGame/GameBoy
// -shaped surface. No BIOS/disk is loaded and no CPU runs yet; launch() is a
// placeholder. The real player (Config with SD-backed disk images, a
// HostSystemInterface bridging Faux86's blit -> the LVGL canvas / ST7789, PC
// speaker -> I2S, ASCII-keyboard -> XT scancodes, and a paced simulate() loop
// that keeps the mesh alive) lands in later steps. T-Deck only; the V4 stubs it.
class DosBox {
public:
  static void launch();               // open the player (SCAFFOLD: no-op)
  static bool isOpen();               // UITask: gate tab bar + trackball nav
  static void steer(int dx, int dy);  // UITask: trackball -> mouse/arrows (later)
  static void keyChar(char c);        // UITask: physical keyboard -> PC keys (later)
  static void close();                // tear down, return to UI
};
