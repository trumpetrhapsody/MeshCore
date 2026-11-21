# RAK 1W MeshCore repeater — optional BTHome BLE telemetry

Experimental firmware, forked from [MeshCore](https://github.com/meshcore-dev/MeshCore).

**Automated build and payload/settings tests PASSED on 2026-11-21. This branch remains NOT hardware-validated.** Keep a known-good stock image and identity/settings backup. [Successful hardened firmware and stock-target build](https://github.com/trumpetrhapsody/MeshCore/actions/runs/44950129255). Hardened image: 35,772 bytes RAM (15.2%) and 415,976 bytes flash (58.4% of its BLE-safe partition); Nordic DFU ZIP verified and included in the build artifact.

## Default behavior and safe disable

**BTHome is OFF by default on a clean installation.** In that state the code never calls `Bluefruit.begin()` during startup, preserving near-stock repeater runtime and power behavior. This is not byte-for-byte identical to stock firmware: the BLE-capable linker layout and additional code remain in flash.

The existing MeshCore RAK3401 repeater configuration, LoRa identity and preferences are untouched. BLE preferences are stored separately at `/bthome.cfg`, using an 8-byte versioned, checksummed LittleFS record and a temporary-file rename. If missing or corrupted, Bluetooth is disabled.

**Why not the native `/prefs.json`?** MeshCore's normal preference save deletes and rewrites that entire file. Keeping two experimental BLE values separate avoids rewriting the critical radio/password/ACL-related configuration just to change a reporting interval. The native `custom` config serializer is a legitimate alternative but has a larger blast radius for an unstable solar repeater.

**Boot-loop protection:** `/bthome.guard` is written before initializing BLE and cleared after one minute of stable operation. If startup is interrupted, the next boot skips BLE but still operates as a LoRa repeater. `bthome status` displays the guard, and an explicit `bthome on` followed by `reboot` clears it and retries. An early brownout or manual reset can also trigger this conservative fallback; it does not prove Bluetooth caused the reset.

## Commands (USB CLI or authenticated repeater admin over LoRa)

| Command | Function |
| --- | --- |
| `bthome status` | Show saved switch, BLE stack state, broadcast state, and interval |
| `bthome on` | Persistently enable for NEXT boot; does not initialize BLE live |
| `bthome off` | Stop BLE broadcasts immediately, persist OFF; **reboot** to remove BLE background overhead |
| `bthome interval 10` | Persist minutes, valid range 1–1440; applies immediately when reporting |
| `reboot` | Restart repeater to apply a changed enabled/disabled state |
| `start ota` | Enter Nordic BLE DFU bootloader-entry advertising mode (five-minute window when BTHome was initialized) |

Example first-time setup:
1. After a known-good USB boot, confirm MeshCore radio settings and identity are intact.
2. Run `bthome status` (OFF initially), then `bthome interval 10`.
3. Run `bthome on`, then `reboot`.
4. Observe the BLE device `RAK1W-BTHome`, verify battery voltage and internal MCU temperature in Home Assistant.
5. To return to stock-like behavior, run `bthome off` and then `reboot`.

Each telemetry reading is sent via a **five-second, 250 ms BLE advertising burst**, every 10 minutes by default. Home Assistant requires 2.4 GHz BLE reception (local adapter or ESPHome proxy); a 915 MHz LoRa radio cannot receive these broadcasts. Transmissions are unencrypted and not actual battery-cell temperature.

## OTA firmware updates — supported path

MeshCore repeater firmware uses **Nordic Bluetooth DFU OTA**, not LoRa firmware transfer:
1. Prepare the `firmware.zip` artifact matching the `RAK_3401_repeater_bthome` build.
2. Send authenticated repeater command `start ota` (USB or mesh remote admin).
3. The repeater advertises the *connectable* `RAK3401_OTA` device and the Nordic DFU service UUID. The existing Nordic bootloader handles the subsequent update.
4. Use a compatible Nordic DFU app from within Bluetooth range to transfer **firmware.zip**. The command can be sent by LoRa, but the firmware bytes travel over Bluetooth.
5. When completed, verify its original LoRa identity and configuration and recheck BTHome telemetry.

The DFU trigger service is registered at BLE startup when reporting is enabled but **is not connectable until the authenticated `start ota` command**. While BTHome is active, the BLE DFU advertising window automatically expires after five minutes without an active connection and normal telemetry resumes. A user with only a LoRa path cannot upload the firmware: the transfer requires a **local Bluetooth connection**. This does not implement MeshCore's distinct LoRa mOTA system. When telemetry is OFF from boot, the original stock MeshCore `start ota` code is called without modification. A partially initialized SoftDevice blocks a second initialization and returns a USB recovery error instead. There is no remote rescue from a firmware bootloop or a dead Bluetooth radio; USB is the fallback.

### Firmware artifacts

The [Build RAK 1W BTHome repeater](https://github.com/trumpetrhapsody/MeshCore/actions/workflows/build-rak1w-bthome.yml) GitHub Actions workflow builds, tests and uploads `firmware.zip` (Nordic BLE OTA DFU), `firmware.uf2` (USB bootloader), `firmware.bin` and `firmware.hex`. The ZIP is for DFU; **do not use raw UF2 as a BLE DFU image**.

## Failure isolation and energy

BLE advertisement errors are throttled to once per 60 seconds, with telemetry stopped after three consecutive errors. BLE startup is protected by a one-minute flash marker; a failed or interrupted early BLE startup causes the next boot to skip Bluetooth until manually reenabled. The system does not automatically reboot, reformat filesystem, change radio parameters, or erase identity on BLE errors. A successful full-firmware compile does not rule out lower-level SoftDevice failures.

Shorter interval = more BLE radio events. But the SoftDevice's initialized standby overhead likely dominates the incremental power consumption; changing from 10 to 30 minutes should save much less than switching Bluetooth OFF and rebooting. Repeatedly enabling/disabling the Nordic SoftDevice every reporting interval is intentionally avoided due to radio and bootloader stability risks. Benchmark idle current and actual repeater forwarding before rooftop installation.

**Important remaining tests:** real RAK 1W LoRa forwarding during BLE bursts; settings retention, OTA entry and actual DFU ZIP transfer; USB recovery; BTHome discovery; standby current/solar budget. Neither OTA nor electrical draw is hardware verified.

## Related implementations reviewed

- **Ultimate MeshCore** (ESP32 Heltec V4) adds an early-BLE-startup crash guard and delayed stable-boot acknowledgment; this inspired the nRF52 BTHome boot guard, but its Wi-Fi/NimBLE coexistence logic cannot be transplanted onto Nordic hardware.
- **MeshCore T-Beam Hybrid** (ESP32) keeps both full BLE companion and repeater behavior live; significantly greater LoRa/identity/CLI coupling than passive BTHome advertisements. It also explicitly warns not to mistake MCU temperature for battery temperature.
- **Meshtastic nRF52** checks `Bluefruit.begin()` before advertising, and supports stopping BLE advertising without requiring teardown of the SoftDevice. Our telemetry uses the same fail-closed pattern, while a power-saving BLE OFF state requires reboot.
- **MeshCoreNG** emphasizes low-battery guards and conservative default-off features; our feature doesn't override stock RAK low-voltage protection.
- BTHome v2 `0x44` marks the device trigger-based (irregular reporting). We currently retain this intentionally for long, sparse advertising bursts to avoid premature HA unavailability. For strict BTHome periodic reporting semantics `0x40` would be more accurate, at the cost of potentially reporting the device unavailable between infrequent samples. This should be confirmed during HA field tests.

No firmware can guarantee no SoftDevice crash on untested hardware. Even with BLE OFF, custom firmware is not byte-for-byte factory firmware due to the different app flash allocation and additional code.
