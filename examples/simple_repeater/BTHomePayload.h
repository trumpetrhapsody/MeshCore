#pragma once

#include <stddef.h>
#include <stdint.h>

// BTHome v2 unencrypted service data, including UUID 0xFCD2 (little-endian).
// Object 0x00: packet ID; 0x02: signed temperature in 0.01 C;
// 0x0C: battery voltage in 0.001 V. Device info 0x44 marks sparse triggers.
namespace BTHomePayload {
constexpr size_t kMaxLength = 11;

inline size_t encode(uint8_t (&bytes)[kMaxLength], uint8_t packetId,
                     uint16_t batteryMilliVolts, bool temperatureValid,
                     int16_t temperatureCentiCelsius) {
  size_t i = 0;
  bytes[i++] = 0xD2;
  bytes[i++] = 0xFC;
  bytes[i++] = 0x44;
  bytes[i++] = 0x00;
  bytes[i++] = packetId;
  if (temperatureValid) {
    bytes[i++] = 0x02;
    bytes[i++] = static_cast<uint8_t>(temperatureCentiCelsius);
    bytes[i++] = static_cast<uint8_t>(static_cast<uint16_t>(temperatureCentiCelsius) >> 8);
  }
  bytes[i++] = 0x0C;
  bytes[i++] = static_cast<uint8_t>(batteryMilliVolts);
  bytes[i++] = static_cast<uint8_t>(batteryMilliVolts >> 8);
  return i;
}
}  // namespace BTHomePayload
