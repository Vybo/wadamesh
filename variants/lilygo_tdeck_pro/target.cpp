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

// ---------------------------------------------------------------------------
// BQ27220 fuel gauge (I2C 0x55)
// ---------------------------------------------------------------------------
// The T-Deck's battery path does not exist on this board: it reads a 2:1 divider
// on GPIO4, and GPIO4 here is BOARD_LORA_RST. So the pack is read from the gauge
// instead, over the same I2C bus as the touch controller and the keyboard.
//
// Returns false when the gauge did not answer, rather than 0 -- a lost
// transaction on a bus this contended is normal rather than exceptional, and
// encoding it as data is what made the same gauge report a flickering 0% on the
// T-Display P4 (#273). This mirrors variants/tdisplay_p4/target.cpp deliberately;
// same chip, same address, same shared-bus situation.
static bool bq27220ReadU16(uint8_t cmd, uint16_t& out) {
  Wire.beginTransmission(0x55);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return false;   // repeated START, no STOP
  if (Wire.requestFrom(0x55, 2) != 2) return false;
  const uint16_t lo = Wire.read(), hi = Wire.read();
  out = (uint16_t)(lo | (hi << 8));
  return true;
}

// Voltage() = REG 0x08, mV. Rate-limited to 5 s and sanity-windowed, holding the
// last good value across a failed read so the battery UI never flickers.
// The throttle matters: UITask smooths over 20 s, but MyMesh and the Commander
// info panel call straight through.
uint16_t TDeckProBoard::getBattMilliVolts() {
  static uint32_t last_ms = 0;
  static uint16_t last_mv = 3800;      // pre-first-read placeholder
  const uint32_t now = millis();
  if (last_ms == 0 || now - last_ms >= 5000) {
    last_ms = now;
    uint16_t mv = 0;
    if (bq27220ReadU16(0x08, mv) && mv >= 2500 && mv <= 4600) last_mv = mv;
    // No Serial diagnostics here on purpose -- the companion protocol owns the
    // UART on this build. Gauge presence shows up on screen as a plausible
    // voltage instead of the 3800 mV placeholder.
  }
  return last_mv;
}

// StateOfCharge() = REG 0x2C, percent. -1 means "gauge silent", which tells the
// caller to fall back to the voltage curve instead of showing a made-up number.
int TDeckProBoard::getBattStateOfCharge() {
  static uint32_t last_ms = 0;
  static int      last_pct = -1;
  const uint32_t now = millis();
  if (last_ms == 0 || now - last_ms >= 5000) {
    last_ms = now;
    uint16_t soc = 0;
    if (bq27220ReadU16(0x2C, soc) && soc <= 100) last_pct = (int)soc;
  }
  return last_pct;
}

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
