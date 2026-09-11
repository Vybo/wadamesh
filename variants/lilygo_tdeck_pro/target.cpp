#include <Arduino.h>
#include "target.h"
#include "GDEQ031EpdDisplay.h"   // full type: this TU DEFINES `display`
#include <helpers/HardwareRtcClock.h>
#include "helpers/sensors/MicroNMEALocationProvider.h"   // full type: this TU DEFINES `gps`

TDeckProBoard board;

#if defined(P_LORA_SCLK)
  static SPIClass spi;
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

SPIClass* tdeckSharedSPI() {
#if defined(P_LORA_SCLK)
  return &spi;     // already begun (SCLK/MISO/MOSI) by radio.std_init
#else
  return nullptr;
#endif
}

ESP32RTCClock fallback_clock;
// The Pro carries a PCF85063-class part on the I2C bus alongside the charger and
// gauge. Until that is confirmed against hardware this stays on the same
// transparent adapter the T-Deck uses: reads fall through to the software clock
// and writes stop at it when nothing answers, so a board without the chip
// behaves exactly as before. CAP_HARDWARE_RTC stays 0 for this board until the
// part is confirmed, which keeps the cold-boot network time sync on offer.
HardwareRtcClock     hw_rtc(fallback_clock);
ClockFloorRTC        rtc_clock(hw_rtc);
MicroNMEALocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  fallback_clock.begin();
  // Touch (0x1A), TCA8418 keyboard (0x34), BQ25896 charger (0x6B) and BQ27220
  // gauge (0x55) all share this bus. The panel is NOT on it — that is SPI.
  Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL);

  // No generic rtc_clock.begin(Wire) probe here, for the same reason the T-Deck
  // dropped it (issue #383): address-only auto-detection is what misread the
  // Pager's PCF85063A as a PCF8563. A board either drives a documented chip
  // through HardwareRtcClock or has none.

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng); // create new random identity
}
