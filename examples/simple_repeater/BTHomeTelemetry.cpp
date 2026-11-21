#if defined(RAK_3401) && defined(ENABLE_BTHOME_TELEMETRY)

#include <Arduino.h>
#include <bluefruit.h>
#include <math.h>
#include <utility/SoftwareTimer.h>
#include <target.h>
#include "BTHomePayload.h"
#include "BTHomeTelemetry.h"

#ifndef BTHOME_REPORT_INTERVAL_MS
  #define BTHOME_REPORT_INTERVAL_MS 600000UL
#endif
#ifndef BTHOME_BURST_SECONDS
  #define BTHOME_BURST_SECONDS 5
#endif
#ifndef BTHOME_ADV_INTERVAL_UNITS
  #define BTHOME_ADV_INTERVAL_UNITS 400 // 250ms, 0.625ms units
#endif

namespace {
SoftwareTimer periodicTimer;
volatile bool reportDue = false;
bool initialized = false;
bool otaActive = false;
uint8_t packetId = 0;

void onTimer(TimerHandle_t) {
  // Never touch ADC or BLE from the FreeRTOS timer callback.
  reportDue = true;
}

bool sendSnapshot() {
  if (!initialized || otaActive || Bluefruit.Advertising.isRunning()) return false;

  const uint16_t millivolts = board.getBattMilliVolts();
  const float temperature = board.getMCUTemperature();
  const bool valid = isfinite(temperature) && temperature > -327.68f && temperature < 327.67f;
  const int16_t centiC = valid ? static_cast<int16_t>(lroundf(temperature * 100.0f)) : 0;

  uint8_t serviceData[BTHomePayload::kMaxLength];
  const size_t dataLength =
      BTHomePayload::encode(serviceData, packetId++, millivolts, valid, centiC);

  Bluefruit.Advertising.clearData();
  // 0x16: BLE Service Data, 16-bit UUID. The 0xFCD2 UUID is included above.
  if (!Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE) ||
      !Bluefruit.Advertising.addData(0x16, serviceData, static_cast<uint8_t>(dataLength)) ||
      !Bluefruit.Advertising.addName()) return false;
  // With fastTimeout(5), this is a finite five-second advertising burst.
  return Bluefruit.Advertising.start(BTHOME_BURST_SECONDS);
}
} // namespace

namespace BTHomeTelemetry {
bool begin() {
  if (initialized) return true;
  if (otaActive) return false;

  Bluefruit.autoConnLed(false);
  if (!Bluefruit.begin(1, 0)) {
    Serial.println("BTHome: BLE initialization failed; repeater continues");
    return false;
  }
  initialized = true;
  Bluefruit.setTxPower(4);
  Bluefruit.setName("RAK1W-BTHome");
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.setInterval(BTHOME_ADV_INTERVAL_UNITS, BTHOME_ADV_INTERVAL_UNITS);
  Bluefruit.Advertising.setFastTimeout(BTHOME_BURST_SECONDS);

  periodicTimer.begin(BTHOME_REPORT_INTERVAL_MS, onTimer, nullptr, true);
  periodicTimer.start();
  sendSnapshot(); // Allows discovery shortly after each reboot.
  return true;
}

void loop() {
  if (!initialized || otaActive || !reportDue) return;
  if (Bluefruit.Advertising.isRunning()) return;
  reportDue = false;
  if (!sendSnapshot()) reportDue = true;
}

bool prepareForOTA() {
  otaActive = true;
  reportDue = false;
  if (initialized) {
    periodicTimer.stop();
    if (Bluefruit.Advertising.isRunning()) Bluefruit.Advertising.stop();
  }
  return initialized;
}
} // namespace BTHomeTelemetry

#endif // RAK_3401 && ENABLE_BTHOME_TELEMETRY
