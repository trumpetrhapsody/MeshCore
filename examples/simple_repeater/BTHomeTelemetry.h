#pragma once
#if defined(RAK_3401) && defined(ENABLE_BTHOME_TELEMETRY)
namespace BTHomeTelemetry {
// Load settings after LittleFS and the normal repeater have initialized.
// Missing or invalid settings keep BLE disabled; never alters LoRa preferences.
bool begin();
// Nonblocking, best-effort telemetry work in the ordinary repeater loop.
void loop();
// Board-specific CLI commands, shared by USB and authenticated mesh CLI.
bool handleCommand(const char* command, char* reply);
// Whether Bluefruit was successfully initialized on this boot.
bool bleInitialized();
// Reuse initialized SoftDevice to advertise Nordic DFU bootloader entry.
// Return true if reply contains a complete result (including an error).
bool startOTA(char* reply);
}
#endif
