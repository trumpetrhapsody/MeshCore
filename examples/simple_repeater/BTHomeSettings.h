#pragma once
#include <stddef.h>
#include <stdint.h>

// Independent, versioned settings record; does not modify MeshCore prefs/identity.
// Invalid/missing records always fail closed (BLE disabled, 10-minute interval).
namespace BTHomeSettings {
constexpr uint16_t kDefaultMinutes = 10;
constexpr uint16_t kMinMinutes = 1;
constexpr uint16_t kMaxMinutes = 1440;
constexpr size_t kRecordLength = 8;

inline bool validInterval(uint16_t minutes) {
  return minutes >= kMinMinutes && minutes <= kMaxMinutes;
}

inline bool parseMinutes(const char* str, uint16_t& minutes) {
  if (!str || *str == '\0') return false;
  uint32_t value = 0;
  for (; *str; ++str) {
    if (*str < '0' || *str > '9') return false;
    value = value * 10 + static_cast<uint32_t>(*str - '0');
    if (value > kMaxMinutes) return false;
  }
  if (value < kMinMinutes) return false;
  minutes = static_cast<uint16_t>(value);
  return true;
}

inline void encode(uint8_t (&bytes)[kRecordLength], bool enabled,
                   uint16_t minutes) {
  bytes[0] = 'B';
  bytes[1] = 'T';
  bytes[2] = 'H';
  bytes[3] = 2;  // format version
  bytes[4] = enabled ? 1 : 0;
  bytes[5] = static_cast<uint8_t>(minutes);
  bytes[6] = static_cast<uint8_t>(minutes >> 8);
  uint8_t checksum = 0xA5;
  for (size_t i = 0; i < kRecordLength - 1; ++i) checksum ^= bytes[i];
  bytes[7] = checksum;
}

inline bool decode(const uint8_t (&bytes)[kRecordLength],
                   bool& enabled, uint16_t& minutes) {
  if (bytes[0] != 'B' || bytes[1] != 'T' ||
      bytes[2] != 'H' || bytes[3] != 2 || bytes[4] > 1) return false;
  uint8_t checksum = 0xA5;
  for (size_t i = 0; i < kRecordLength - 1; ++i) checksum ^= bytes[i];
  if (checksum != bytes[7]) return false;
  const uint16_t value = static_cast<uint16_t>(bytes[5]) |
                         (static_cast<uint16_t>(bytes[6]) << 8);
  if (!validInterval(value)) return false;
  enabled = bytes[4] != 0;
  minutes = value;
  return true;
}
} // namespace BTHomeSettings
