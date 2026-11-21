# RAK 1W repeater BTHome Bluetooth telemetry

Experimental extension for RAK3401 + RAK13302 (WisMesh 1W Booster), based on [MeshCore](https://github.com/meshcore-dev/MeshCore).

**Status:** BTHome packet encoder tested locally. Complete firmware compile, repeater function, Bluetooth DFU, flash layout, and power draw are **not yet validated on hardware**.

## Features
- Normal LoRa repeater path and its identity/settings code are retained.
- Local, unencrypted BTHome v2 BLE service-data broadcasts with battery millivolts and nRF52840 MCU temperature (not cell temperature).
- One snapshot shortly after boot, then once per 600 seconds.
- Each snapshot is broadcast for five seconds, with 250 ms intervals.
- No companion node or LoRa airtime is used for these readings.
- BTHome device-information byte 0x44 has trigger-based reporting set.
- Board-specific build environment `RAK_3401_repeater_bthome`; stock `RAK_3401_repeater` remains unchanged.
- Existing BLE DFU handoff is overridden only for this target to reuse initialized BLE stack.

## Building
Run from repository root:

```sh
pio run -e RAK_3401_repeater_bthome
pio run -e RAK_3401_repeater_bthome -t create_uf2
```

The dedicated GitHub Actions workflow attempts to compile and publish .hex, .bin and .uf2 artifacts. For forks, Actions might need manual enabling.

## Home Assistant
Use an existing Bluetooth adapter or an ESPHome Bluetooth proxy that is physically within BLE reception range. The expected BTHome device name is `RAK1W-BTHome`. The existing LoRa antenna does not carry Bluetooth.

## Known limitations
- BLE values and device address are visible to nearby listeners (unencrypted).
- Intermittent scanning can miss short bursts; BLE reception must be checked.
- SoftDevice/FreeRTOS may change solar battery drain: monitor battery voltage across days.
- DFU, reboot, radio forwarding, device identity, Settings, and bootloader recovery need bench verification.
- Keep a known-good stock RAK3401 firmware and back up repeater identity/settings before any flashing.

## Upstream base
`meshcore-dev/MeshCore` commit `75b04ef06def54b348400cb1b138afb6749ffd4f`.
