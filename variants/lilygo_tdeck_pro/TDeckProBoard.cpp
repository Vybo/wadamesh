#include <Arduino.h>
#include "TDeckProBoard.h"

uint32_t deviceOnline = 0x00;

void TDeckProBoard::begin() {
  ESP32Board::begin();

  // The SX1262 section sits behind its own rail switch on this board. It has to
  // come up BEFORE radio_init() runs std_init(), otherwise the chip simply
  // isn't there yet and the radio reports as missing. The T-Deck has no
  // equivalent — its PIN_PERF_POWERON covers the whole peripheral block.
#if defined(PIN_LORA_POWER_EN)
  pinMode(PIN_LORA_POWER_EN, OUTPUT);
  digitalWrite(PIN_LORA_POWER_EN, HIGH);
  delay(10);
#endif

  // GNSS rail. Held off until asked for, same as the radio: the MIA-M10Q draws
  // continuously once powered and this board has no ADC-based battery reading
  // to notice it with.
#if defined(PIN_GPS_EN)
  pinMode(PIN_GPS_EN, OUTPUT);
  digitalWrite(PIN_GPS_EN, HIGH);
#endif

  pinMode(PIN_USER_BTN, INPUT_PULLUP);   // side button, active low
  pinMode(P_LORA_MISO, INPUT_PULLUP);

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    long wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1 << P_LORA_DIO_1)) {
      startup_reason = BD_STARTUP_RX_PACKET;
    }
    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}
