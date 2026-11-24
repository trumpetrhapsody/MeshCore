#if defined(RAK_3401) && defined(ENABLE_BTHOME_TELEMETRY)

#include <Arduino.h>
#include <InternalFileSystem.h>
#include <bluefruit.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <utility/SoftwareTimer.h>
#include <target.h>
#include "BTHomePayload.h"
#include "BTHomeSettings.h"
#include "BTHomeTelemetry.h"

#ifndef BTHOME_BURST_SECONDS
  #define BTHOME_BURST_SECONDS 5
#endif
#ifndef BTHOME_ADV_INTERVAL_UNITS
  #define BTHOME_ADV_INTERVAL_UNITS 400 // 250ms in 0.625ms units
#endif

namespace {
constexpr const char* kSettingsFile = "/bthome.cfg";
constexpr const char* kTempFile = "/bthome.tmp";
constexpr const char* kBootGuardFile = "/bthome.guard";
constexpr uint32_t kBootStableMs = 60000UL;
constexpr uint16_t kOTAMaxSeconds = 300; // Limit unconnected BLE DFU advertising to five minutes.

constexpr uint32_t kFailureRetryMs = 60000UL;
constexpr uint8_t kMaxFailures = 3;
constexpr size_t kReplyLength = 160;

SoftwareTimer periodicTimer;
BLEDfu dfuService;
volatile bool reportDue = false;
bool configuredEnabled = false;   // persisted choice
uint16_t intervalMinutes = BTHomeSettings::kDefaultMinutes;
bool loaded = false;
bool corruptSettings = false;
bool initialized = false;         // Bluefruit / SoftDevice successfully started
bool dfuReady = false;
bool reporting = false;           // timer and BLE bursts enabled
bool otaMode = false;
uint32_t otaStartedAt = 0;
bool guardBlocked = false; // Persists after a crash until bthome on/off is explicitly used.
bool guardArmed = false;
uint32_t guardArmedAt = 0;
uint32_t guardClearRetryAt = 0;
uint8_t packetId = 0;
uint8_t consecutiveFailures = 0;
uint32_t nextRetryAt = 0;

void onTimer(TimerHandle_t) {
  reportDue = true;  // no BLE, filesystem or ADC work in RTOS timer callback
}

void stopReports() {
  reporting = false;
  reportDue = false;
  if (periodicTimer.getHandle()) periodicTimer.stop();
  if (initialized && Bluefruit.Advertising.isRunning()) {
    Bluefruit.Advertising.stop();
  }
}

void loadSettings() {
  if (loaded) return;
  loaded = true;
  configuredEnabled = false; // Fail closed on erased, corrupt, unreadable settings.
  intervalMinutes = BTHomeSettings::kDefaultMinutes;
  if (!InternalFS.exists(kSettingsFile)) return;

  Adafruit_LittleFS_Namespace::File file = InternalFS.open(kSettingsFile, Adafruit_LittleFS_Namespace::FILE_O_READ);
  if (!file) {
    corruptSettings = true;
    return;
  }
  uint8_t bytes[BTHomeSettings::kRecordLength] = {};
  const int n = file.read(bytes, sizeof(bytes));
  const bool exact = n == static_cast<int>(sizeof(bytes)) && !file.available();
  file.close();

  bool enabled = false;
  uint16_t minutes = BTHomeSettings::kDefaultMinutes;
  if (!exact || !BTHomeSettings::decode(bytes, enabled, minutes)) {
    corruptSettings = true;
    Serial.println("BTHome: invalid configuration; BLE OFF (safe default)");
    return;
  }
  configuredEnabled = enabled;
  intervalMinutes = minutes;
}

bool persistSettings(bool enable, uint16_t minutes) {
  uint8_t bytes[BTHomeSettings::kRecordLength] = {};
  BTHomeSettings::encode(bytes, enable, minutes);

  // LittleFS rename is atomic. The previous configuration is retained if
  // power fails before the new file is committed.
  if (InternalFS.exists(kTempFile)) InternalFS.remove(kTempFile);
  Adafruit_LittleFS_Namespace::File file = InternalFS.open(kTempFile, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
  if (!file) return false;
  const size_t written = file.write(bytes, sizeof(bytes));
  file.flush();
  file.close();
  if (written != sizeof(bytes)) {
    InternalFS.remove(kTempFile);
    return false;
  }
  if (!InternalFS.rename(kTempFile, kSettingsFile)) {
    InternalFS.remove(kTempFile);
    return false;
  }
  corruptSettings = false;
  return true;
}

// The marker is created *before* BLE setup. A crash, watchdog reboot, or
// power cut during the initial stabilization window leaves it present, so the
// next boot skips Bluetooth and keeps the LoRa repeater available.
bool armBootGuard() {
  if (InternalFS.exists(kBootGuardFile)) {
    guardBlocked = true;
    return false;
  }
  Adafruit_LittleFS_Namespace::File file =
      InternalFS.open(kBootGuardFile, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
  if (!file) return false;
  const size_t n = file.write(static_cast<uint8_t>(0xB7));
  file.flush();
  file.close();
  if (n != 1) return false;
  guardArmed = true;
  guardArmedAt = millis();
  return true;
}

void clearBootGuard() {
  if (!InternalFS.exists(kBootGuardFile) || InternalFS.remove(kBootGuardFile)) {
    guardBlocked = false;
    guardArmed = false;
  } else {
    // Avoid spinning on flash writes when the filesystem is unhealthy.
    guardClearRetryAt = millis() + kBootStableMs;
    Serial.println("BTHome: could not clear boot guard; retrying later");
  }
}

bool sendSnapshot() {
  if (!initialized || !reporting || otaMode) return false;
  if (Bluefruit.Advertising.isRunning()) return true;

  const uint16_t mv = board.getBattMilliVolts();
  const float temp = board.getMCUTemperature();
  const bool tempValid = isfinite(temp) && temp > -327.68f && temp < 327.67f;
  const int16_t centiC = tempValid ?
      static_cast<int16_t>(lroundf(temp * 100.0f)) : 0;

  uint8_t serviceData[BTHomePayload::kMaxLength] = {};
  const size_t len = BTHomePayload::encode(
      serviceData, packetId, mv, tempValid, centiC);

  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  if (!Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE) ||
      !Bluefruit.Advertising.addData(0x16, serviceData, static_cast<uint8_t>(len)) ||
      !Bluefruit.Advertising.addName() ||
      !Bluefruit.Advertising.start(BTHOME_BURST_SECONDS)) return false;
  ++packetId;
  return true;
}

void handleFailure() {
  ++consecutiveFailures;
  if (consecutiveFailures >= kMaxFailures) {
    Serial.println("BTHome: repeated BLE advertising failures; disabling telemetry until reboot");
    stopReports(); // Never reboot and never interrupt LoRa forwarding.
  } else {
    Serial.println("BTHome: advertising failed; will retry after 60 seconds");
    nextRetryAt = millis() + kFailureRetryMs;
    reportDue = true;
  }
}

void restoreAdvertisingMode() {
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(BTHOME_ADV_INTERVAL_UNITS, BTHOME_ADV_INTERVAL_UNITS);
  Bluefruit.Advertising.setFastTimeout(BTHOME_BURST_SECONDS);
  Bluefruit.setName("RAK1W-BTHome");
}

void reportStatus(char* reply) {
  snprintf(reply, kReplyLength, "BTHome: saved=%s BLE=%s reports=%s every=%umin%s%s",
           configuredEnabled ? "on" : "off",
           initialized ? "on" : "off",
           reporting ? "on" : "off",
           static_cast<unsigned>(intervalMinutes),
           corruptSettings ? " (config invalid)" : "",
           guardBlocked ? " (boot guard blocked BLE; bthome on + reboot)" : "");
}
} // namespace

namespace BTHomeTelemetry {
bool begin() {
  loadSettings();
  if (!configuredEnabled) {
    Serial.println("BTHome: OFF; starting repeater without Bluetooth stack");
    return false;
  }

  if (!armBootGuard()) {
    Serial.println(guardBlocked
        ? "BTHome: BLE boot guard active after interrupted startup; LoRa only"
        : "BTHome: cannot arm BLE boot guard; skipping Bluetooth for safety");
    return false;
  }

  Bluefruit.autoConnLed(false);
  // Do not change the system-wide shared radio or filesystem settings.
  if (!Bluefruit.begin(1, 0)) {
    Serial.println("BTHome: Bluefruit init failed; LoRa remains active");
    return false;
  }
  initialized = true;
  Bluefruit.setTxPower(4);
  Bluefruit.setName("RAK1W-BTHome");

  // Register the Nordic DFU entry service once at boot. BLE is non-connectable
  // during normal telemetry, so this service isn't remotely accessible until
  // an authenticated MeshCore 'start ota' command switches modes.
  dfuReady = dfuService.begin() == ERROR_NONE;
  if (!dfuReady) {
    Serial.println("BTHome: DFU service registration failed; BLE telemetry disabled");
    return false;
  }

  restoreAdvertisingMode();

  periodicTimer.begin(static_cast<uint32_t>(intervalMinutes) * 60000UL, onTimer);
  if (!periodicTimer.getHandle()) {
    Serial.println("BTHome: periodic timer allocation failed; BLE telemetry disabled");
    return false;
  }
  reporting = true;
  periodicTimer.start();
  reportDue = true; // The first report is scheduled immediately.
  Serial.printf("BTHome: enabled, interval %u min\n",
                static_cast<unsigned>(intervalMinutes));
  return true;
}

void loop() {
  // Clearing the marker after a full minute of stable LoRa+BLE operation
  // avoids a persistent reboot loop if enabling BLE triggers a crash.
  if (guardArmed &&
      static_cast<uint32_t>(millis() - guardArmedAt) >= kBootStableMs &&
      (guardClearRetryAt == 0 ||
       static_cast<int32_t>(millis() - guardClearRetryAt) >= 0)) {
    clearBootGuard();
  }

  // BLE DFU entry is deliberately temporary. It must not advertise forever
  // at a high frequency after an abandoned update.
  if (otaMode) {
    if (static_cast<uint32_t>(millis() - otaStartedAt) >=
          static_cast<uint32_t>(kOTAMaxSeconds) * 1000UL &&
        !Bluefruit.Periph.connected()) {
      Bluefruit.Advertising.restartOnDisconnect(false);
      if (Bluefruit.Advertising.isRunning()) Bluefruit.Advertising.stop();
      otaMode = false;
      restoreAdvertisingMode();
      if (configuredEnabled && dfuReady && periodicTimer.getHandle()) {
        reporting = true;
        periodicTimer.start();
        reportDue = true;
      }
      Serial.println("BTHome: BLE OTA window expired; restored normal mode");
    }
    return;
  }
  if (!reporting || !reportDue) return;
  if (Bluefruit.Advertising.isRunning()) return;
  if (consecutiveFailures && static_cast<int32_t>(millis() - nextRetryAt) < 0) {
    return;
  }
  reportDue = false;
  if (!sendSnapshot()) {
    handleFailure();
  } else {
    consecutiveFailures = 0;
    nextRetryAt = 0;
  }
}

bool handleCommand(const char* command, char* reply) {
  if (strncmp(command, "bthome", 6) != 0 ||
      (command[6] != '\0' && command[6] != ' ')) return false;
  loadSettings();

  const char* option = command + 6;
  while (*option == ' ') ++option;
  if (*option == '\0' || strcmp(option, "status") == 0) {
    reportStatus(reply);
    return true;
  }
  if (otaMode) {
    snprintf(reply, kReplyLength, "ERR: OTA in progress; BTHome settings locked");
    return true;
  }
  if (strcmp(option, "on") == 0 || strcmp(option, "off") == 0) {
    const bool enabled = strcmp(option, "on") == 0;
    if (configuredEnabled != enabled && !persistSettings(enabled, intervalMinutes)) {
      snprintf(reply, kReplyLength, "ERR: unable to save BLE setting");
      return true;
    }
    configuredEnabled = enabled;
    // A manual toggle also clears the safety latch so the next reboot can
    // intentionally retry BLE. On a power-cut/brownout this favors LoRa uptime.
    if (guardBlocked || guardArmed || InternalFS.exists(kBootGuardFile)) {
      if (!InternalFS.remove(kBootGuardFile) && InternalFS.exists(kBootGuardFile)) {
        snprintf(reply, kReplyLength,
                 "ERR: BLE setting saved, but boot guard could not be cleared");
        return true;
      }
      guardBlocked = false;
      guardArmed = false;
    }
    if (!enabled) stopReports(); // Stop on-air output immediately.
    snprintf(reply, kReplyLength, "OK: BTHome %s saved; reboot to %s Bluetooth stack",
             enabled ? "on" : "off",
             enabled ? "initialize" : "disable");
    return true;
  }
  if (strncmp(option, "interval ", 9) == 0) {
    uint16_t minutes = 0;
    if (!BTHomeSettings::parseMinutes(option + 9, minutes)) {
      snprintf(reply, kReplyLength, "ERR: interval must be 1-1440 minutes");
      return true;
    }
    if (minutes != intervalMinutes && !persistSettings(configuredEnabled, minutes)) {
      snprintf(reply, kReplyLength, "ERR: unable to save interval");
      return true;
    }
    intervalMinutes = minutes;
    if (reporting && periodicTimer.getHandle()) {
      periodicTimer.setPeriod(static_cast<uint32_t>(minutes) * 60000UL);
    }
    snprintf(reply, kReplyLength, "OK: BTHome every %u min (saved)",
             static_cast<unsigned>(intervalMinutes));
    return true;
  }
  snprintf(reply, kReplyLength,
           "Usage: bthome [status|on|off|interval 1-1440]");
  return true;
}

bool bleInitialized() {
  return initialized;
}

bool startOTA(char* reply) {
  if (!initialized || !dfuReady) {
    snprintf(reply, kReplyLength, "ERR: BLE DFU unavailable; use USB recovery");
    return true; // Prevent generic CLI from overwriting this error.
  }
  if (otaMode) {
    snprintf(reply, kReplyLength, "OK: BLE OTA mode already active");
    return true;
  }

  // Free the one legacy advertisement set and switch from nonconnectable
  // BTHome to connectable Nordic DFU. No second Bluefruit.begin() is called.
  stopReports();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.setTxPower(4);
  Bluefruit.setName("RAK3401_OTA");
  // DFU service UUID in advertisements, name in scan response for discovery.
  const bool advOK =
      Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE) &&
      Bluefruit.Advertising.addTxPower() &&
      Bluefruit.Advertising.addService(dfuService) &&
      Bluefruit.ScanResponse.addName() &&
      Bluefruit.Advertising.start(kOTAMaxSeconds);
  if (!advOK) {
    Serial.println("BTHome: unable to enter BLE OTA; restoring telemetry mode");
    restoreAdvertisingMode();
    if (configuredEnabled && periodicTimer.getHandle()) {
      reporting = true;
      periodicTimer.start();
      reportDue = true;
    }
    snprintf(reply, kReplyLength, "ERR: BLE OTA failed; USB recovery available");
    return true;
  }

  otaMode = true;
  otaStartedAt = millis();
  // OTA may reboot into the bootloader before the normal one-minute guard
  // window; a successful DFU initiation must not count as a BLE startup crash.
  if (guardArmed) clearBootGuard();
  uint8_t mac[6] = {};
  Bluefruit.getAddr(mac);
  snprintf(reply, kReplyLength, "OK - BLE DFU 5min; mac: %02X:%02X:%02X:%02X:%02X:%02X",
           mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
  return true;
}
} // namespace BTHomeTelemetry

#endif // RAK_3401 && ENABLE_BTHOME_TELEMETRY
