#include <Arduino.h>
#include <Wire.h>

#include "RAK3401Board.h"

#ifdef ENABLE_BTHOME_TELEMETRY
#include <bluefruit.h>
#include "BTHomeTelemetry.h"
#endif

#ifdef NRF52_POWER_MANAGEMENT
// Static configuration for power management
// Values set in variant.h defines
const PowerMgtConfig power_config = {
  .lpcomp_ain_channel = PWRMGT_LPCOMP_AIN,
  .lpcomp_refsel = PWRMGT_LPCOMP_REFSEL,
  .voltage_bootlock = PWRMGT_VOLTAGE_BOOTLOCK
};

void RAK3401Board::initiateShutdown(uint8_t reason) {
  // Disable SKY66122 FEM (CSD+CPS LOW = shutdown, <1 uA)
  digitalWrite(SX126X_POWER_EN, LOW);

  // Disable 3V3 switched peripherals and 5V boost
  digitalWrite(PIN_3V3_EN, LOW);

  if (reason == SHUTDOWN_REASON_LOW_VOLTAGE ||
      reason == SHUTDOWN_REASON_BOOT_PROTECT) {
    configureVoltageWake(power_config.lpcomp_ain_channel, power_config.lpcomp_refsel);
  }

  enterSystemOff(reason);
}
#endif

void RAK3401Board::begin() {
  NRF52BoardDCDC::begin();
  pinMode(PIN_VBAT_READ, INPUT);
#ifdef PIN_USER_BTN
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
#endif

#ifdef PIN_USER_BTN_ANA
  pinMode(PIN_USER_BTN_ANA, INPUT_PULLUP);
#endif

#if defined(PIN_BOARD_SDA) && defined(PIN_BOARD_SCL)
  Wire.setPins(PIN_BOARD_SDA, PIN_BOARD_SCL);
#endif

  Wire.begin();

  // PIN_3V3_EN (WB_IO2, P0.34) controls the 3V3_S switched peripheral rail
  // AND the 5V boost regulator (U5) on the RAK13302 that powers the SKY66122 PA.
  // Must stay HIGH during radio operation — do not toggle for power saving.
  pinMode(PIN_3V3_EN, OUTPUT);
  digitalWrite(PIN_3V3_EN, HIGH);

  // Enable SKY66122-11 FEM on the RAK13302 module.
  // CSD and CPS are tied together on the RAK13302 PCB, routed to IO3 (P0.21).
  // HIGH = FEM active (LNA for RX, PA path available for TX).
  // TX/RX switching (CTX) is handled by SX1262 DIO2 via SetDIO2AsRfSwitchCtrl.
  pinMode(SX126X_POWER_EN, OUTPUT);
#ifdef NRF52_POWER_MANAGEMENT
  // Boot voltage protection check (may not return if voltage too low)
  // We need to call this after we configure SX126X_POWER_EN as output but before we pull high
  checkBootVoltage(&power_config);
#endif
  digitalWrite(SX126X_POWER_EN, HIGH);
  delay(1);  // SKY66122 turn-on settling time (tON = 3us typ)
}

#ifdef ENABLE_BTHOME_TELEMETRY
#include <nrf_soc.h>

// Reuse MeshCore's board extension point; also available to authorized remote
// repeater admins (and USB CLI). Do not change the shared CommonCLI parser.
bool RAK3401Board::handleCommand(const char* command, uint32_t sender_timestamp,
                                 char* reply) {
  (void)sender_timestamp;
  return BTHomeTelemetry::handleCommand(command, reply);
}

bool RAK3401Board::startOTAUpdate(const char* id, char reply[]) {
  if (BTHomeTelemetry::bleInitialized()) {
    // Bluefruit was initialized earlier; never initialize the SoftDevice twice.
    return BTHomeTelemetry::startOTA(reply);
  }

  uint8_t softdeviceEnabled = 0;
  sd_softdevice_is_enabled(&softdeviceEnabled);
  if (softdeviceEnabled) {
    // Partial BLE initialization: re-entering Bluefruit.begin() can crash.
    strcpy(reply, "ERR: BLE stack partially active; use USB recovery");
    return true; // Return handled, so CommonCLI preserves the explanation.
  }
  // Telemetry disabled at boot: exactly the upstream MeshCore nRF52 BLE OTA path.
  return NRF52Board::startOTAUpdate(id, reply);
}
#endif // ENABLE_BTHOME_TELEMETRY
