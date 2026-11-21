#pragma once

#if defined(RAK_3401) && defined(ENABLE_BTHOME_TELEMETRY)
namespace BTHomeTelemetry {
// Initializes non-connectable BLE reporting, including a startup report.
bool begin();
// Called from the regular repeater loop; does not delay LoRa forwarding.
void loop();
// Stops periodic reports so OTA/DFU can reuse the existing BLE SoftDevice.
// Returns true only if the telemetry BLE stack was initialized.
bool prepareForOTA();
}  // namespace BTHomeTelemetry
#endif
