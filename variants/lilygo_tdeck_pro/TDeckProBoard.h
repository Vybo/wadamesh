#pragma once

#include <Wire.h>
#include <Arduino.h>
#include "helpers/ESP32Board.h"
#include <driver/rtc_io.h>

// LilyGo T-Deck Pro. Same ESP32-S3 family as the T-Deck, but the board-level
// deltas are real and none of them are cosmetic:
//
//  * No battery ADC. The T-Deck reads a 2:1 divider on GPIO4; on the Pro GPIO4
//    is BOARD_LORA_RST, so reading it as an ADC would be reading the radio's
//    reset line. Charge and gauge live on I2C instead (BQ25896 charger +
//    BQ27220 fuel gauge). Until that driver lands, getBattMilliVolts() reports
//    0 rather than a plausible-looking lie — the UI already renders an unknown
//    battery, and a wrong percentage is worse than a missing one.
//  * The radio module is rail-switched: GPIO46 powers the whole SX1262 section
//    and has to be asserted before any radio init, or std_init() sees a dead
//    chip.
//  * DIO1 is GPIO5, which IS in the S3 RTC-GPIO domain, so the deep-sleep
//    wake-on-packet path the T-Deck uses works here unchanged.
class TDeckProBoard : public ESP32Board {
public:
  void begin();

  void enterDeepSleep(uint32_t secs, int pin_wake_btn) {
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);
    rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);

    if (pin_wake_btn < 0) {
      esp_sleep_enable_ext1_wakeup((1L << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH);
    } else {
      esp_sleep_enable_ext1_wakeup((1L << P_LORA_DIO_1) | (1L << pin_wake_btn),
                                   ESP_EXT1_WAKEUP_ANY_HIGH);
    }
    if (secs > 0) esp_sleep_enable_timer_wakeup(secs * 1000000);
    esp_deep_sleep_start();
  }

  // Both come from the BQ27220 fuel gauge on the shared I2C bus (0x55), NOT from
  // an ADC divider -- see the class comment. Defined in target.cpp because they
  // need Wire, which this header deliberately does not drag into every TU that
  // includes target.h.
  uint16_t getBattMilliVolts();

  // Coulomb-counted percentage from the gauge, or -1 when it does not answer.
  // Preferred over the generic voltage curve: terminal voltage is charger-driven,
  // so on USB it pins to 100% while the pack is flat (the #273 failure on the
  // T-Display P4, which carries the same gauge).
  int getBattStateOfCharge();

  const char* getManufacturerName() const { return "LilyGo T-Deck Pro"; }
};
