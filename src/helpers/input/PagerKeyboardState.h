// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

#include "LatchedModifier.h"

class PagerKeyboardState {
 public:
  // Both boards drive a TCA8418 as a 4x10 matrix and both decode the raw event
  // identically ((raw & 0x7F) - 1, bit 7 = press), so only the key POSITIONS
  // differ -- and they differ completely. Reusing the pager's positions on the
  // Pro would be worse than having no keymap at all: the Pro's Enter would act
  // as Alt and its 'z' as Shift, silently eating two real keys.
  static constexpr uint8_t ROWS = 4;
  static constexpr uint8_t COLS = 10;
#if defined(HAS_TDECK_PRO)
  // LilyGo T-Deck Pro, 35 keys. Cross-confirmed by two independent sources that
  // agree exactly once LilyGo's column reversal is undone: Meshtastic's
  // src/input/TDeckProKeyboard.cpp (labelled, raw flat order) and LilyGo's own
  // examples/factory/peri_keypad.cpp (geometry, column-reversed).
  // UNVERIFIED ON HARDWARE -- see variants/lilygo_tdeck_pro/PORT_NOTES.md.
  static constexpr uint8_t ALT_POS       = 29;
  static constexpr uint8_t SHIFT_POS     = 30;   // right shift
  static constexpr uint8_t SHIFT2_POS    = 34;   // left shift -- the Pro has two
  static constexpr uint8_t SYM_POS       = 31;   // no pager equivalent
  static constexpr uint8_t BACKSPACE_POS = 10;
  static constexpr uint8_t SPACE_POS     = 32;
#else
  static constexpr uint8_t ALT_POS = 2 * COLS;
  static constexpr uint8_t SHIFT_POS = 2 * COLS + 8;
  static constexpr uint8_t BACKSPACE_POS = 2 * COLS + 9;
  static constexpr uint8_t SPACE_POS = 3 * COLS;
  // The pager has neither of these. Park them on values no 4x10 key code can
  // reach, so the shared branches below behave exactly as they did before and
  // test/test_pager_keyboard_state.cpp keeps passing unchanged.
  static constexpr uint8_t SHIFT2_POS = 0xFF;
  static constexpr uint8_t SYM_POS    = 0xFF;
#endif

  uint8_t event(uint8_t code, bool pressed, uint32_t now_ms) {
#if defined(HAS_TDECK_PRO)
    // Indexed [code / COLS][code % COLS] with NO column reversal, matching the
    // decode below. The physical order runs p..q, not q..p -- that is the
    // hardware, not a transcription slip. (LilyGo's own table is the mirror of
    // this because their driver reverses the column index; ours does not.)
    static const char base[ROWS][COLS] = {
      {'p', 'o', 'i', 'u', 'y', 't', 'r', 'e', 'w', 'q'},
      {'\0', 'l', 'k', 'j', 'h', 'g', 'f', 'd', 's', 'a'},  // [1][0] = Backspace (BACKSPACE_POS)
      {'\r', '$', 'm', 'n', 'b', 'v', 'c', 'x', 'z', '\0'}, // [2][0] Enter, [2][9] Alt
      {'\0', '\0', ' ', '\0', '\0', '\0', '\0', '\0', '\0', '\0'}, // R_Shift, Sym, Space, Mic, L_Shift
    };
    static const char symbols[ROWS][COLS] = {
      {'@', '+', '-', '_', ')', '(', '3', '2', '1', '#'},
      {'\0', '"', '\'', ';', ':', '/', '6', '5', '4', '*'},
      {'\0', '\0', '.', ',', '!', '?', '9', '8', '7', '\0'},
      {'\0', '\0', ' ', '0', '\0', '\0', '\0', '\0', '\0', '\0'},  // Mic = '0' on the symbol layer
    };
#else
    static const char base[ROWS][COLS] = {
      {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
      {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\r'},
      {'\0', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '\0', '\0'},
      {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
    };
    static const char symbols[ROWS][COLS] = {
      {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
      {'*', '/', '+', '-', '=', ':', '\'', '"', '@', '\0'},
      {'\0', '_', '$', ';', '?', '!', ',', '.', '\0', '\0'},
      {' ',  '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'},
    };
#endif

    // The Pro's Sym key selects the same symbol layer the pager reaches through
    // Alt, so it feeds the SAME LatchedModifier -- hold, one-shot on a solo tap,
    // lock on a double tap -- instead of growing a second mechanism. Physical
    // Alt stays the modifier for the chords below (Alt+Shift, Alt+Backspace).
    if (code == ALT_POS || code == SYM_POS) {
      if (pressed) alt_.press();
      else         alt_.release(now_ms);
      return 0;
    }
    if (code == SHIFT_POS || code == SHIFT2_POS) {
      if (pressed) {
        if (alt_.held()) {
          alt_.markHeldUsed();
          alt_shift_chord_pending_ = true;
        } else {
          shift_held_ = true;
        }
      } else {
        shift_held_ = false;
      }
      return 0;
    }
    if (code == BACKSPACE_POS) {
      if (pressed) {
        if (alt_.held()) {
          alt_.markHeldUsed();
          alt_backspace_chord_pending_ = true;
        } else {
          alt_.consumeForKey();
          backspace_held_ = true;
          return '\b';
        }
      } else {
        backspace_held_ = false;
      }
      return 0;
    }
    if (code == SPACE_POS) {
      space_held_ = pressed;
      if (pressed) {
        alt_.consumeForKey();
        return ' ';
      }
      return 0;
    }
    if (!pressed) return 0;

    const uint8_t row = code / COLS;
    const uint8_t col = code % COLS;
    if (row >= ROWS) return 0;
    const bool symbol_layer = alt_.consumeForKey();
    char key = symbol_layer ? symbols[row][col] : base[row][col];
    if (key == '\0') return 0;
    if ((caps_ || shift_held_) && !symbol_layer && key >= 'a' && key <= 'z') key -= 32;
    return (uint8_t)key;
  }

  bool altHeld() const { return alt_.held(); }
  void markAltUsed() { alt_.markHeldUsed(); }
  void discardAlt() {
    alt_.discard();
    alt_shift_chord_pending_ = false;
    alt_backspace_chord_pending_ = false;
  }
  bool backspaceHeld() const { return backspace_held_; }
  bool spaceHeld() const { return space_held_; }
  bool consumeAltShiftChord() {
    const bool pending = alt_shift_chord_pending_;
    alt_shift_chord_pending_ = false;
    return pending;
  }
  void toggleCaps() { caps_ = !caps_; }
  bool consumeAltBackspaceChord() {
    const bool pending = alt_backspace_chord_pending_;
    alt_backspace_chord_pending_ = false;
    return pending;
  }

 private:
  LatchedModifier alt_;
  bool caps_ = false;
  bool shift_held_ = false;
  bool alt_shift_chord_pending_ = false;
  bool alt_backspace_chord_pending_ = false;
  bool backspace_held_ = false;
  bool space_held_ = false;
};