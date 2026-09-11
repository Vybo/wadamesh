#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <TDeckProBoard.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include "../../src/helpers/ClockFloorRTC.h"   // monotonic send-timestamp floor (issue #89)
#include <helpers/SensorManager.h>
#ifdef DISPLAY_CLASS
  // The vendored core's ESP32Board.cpp does `#include <target.h>` and calls
  // display.turnOff(), so it needs the COMPLETE type — a forward declaration is
  // not enough. Its compile does see this variant directory (`-I variants/...`
  // is a global build flag), so the include itself resolves.
  //
  // What does NOT resolve is PlatformIO's LDF *scan* of this same chain, which
  // runs in the core library's own include scope. It aborts at this line and
  // silently drops every include AFTER it in this file. That is why
  // MicroNMEALocationProvider.h is no longer included here (see below) and why
  // GDEQ031EpdDisplay.h is pimpl'd so it does not pull GxEPD2 in turn. Keep any
  // new include in this file ABOVE this block, or give it the same treatment.
  #include <GDEQ031EpdDisplay.h>
  #include <helpers/ui/MomentaryButton.h>
#endif
#include "helpers/sensors/EnvironmentSensorManager.h"
// MicroNMEALocationProvider.h is deliberately NOT included here — nothing in this
// header needs it (the `gps` object is internal to target.cpp; only `sensors` is
// extern'd). Keeping it out matters for the same LDF reason as the display class
// above: the vendored core scans this file, and its own
// helpers/sensors/MicroNMEALocationProvider.h pulls <MicroNMEA.h>, which is not
// in the core library's resolved dependency set on this env.


extern TDeckProBoard board;
extern WRAPPER_CLASS radio_driver;
extern RADIO_CLASS radio;   // raw SX1262 — driven directly by the Spectrum analyzer sweep
extern ClockFloorRTC rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

// Shared SPI bus instance (SCLK36/MISO47/MOSI33), already begun for the LoRa
// radio. The e-paper panel (CS=34) and the microSD (CS=48) both reuse it, so
// neither fights the radio for the bus.
//
// NOTE the naming: the SD mount path in main.cpp/UITask.cpp calls
// tdeckSharedSPI() by that exact name across every board that shares a bus this
// way (T-Deck, V4-R8, Pager, M9). Keeping the name means the Pro joins those
// OR-lists without a new accessor.
SPIClass* tdeckSharedSPI();

bool radio_init();
mesh::LocalIdentity radio_new_identity();
