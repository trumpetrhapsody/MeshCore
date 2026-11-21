#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include "../../examples/simple_repeater/BTHomeSettings.h"

int main() {
  uint8_t bytes[BTHomeSettings::kRecordLength] = {};
  bool enabled = false;
  uint16_t minutes = 0;
  BTHomeSettings::encode(bytes, true, 10);
  assert(BTHomeSettings::decode(bytes, enabled, minutes) && enabled && minutes == 10);
  BTHomeSettings::encode(bytes, false, 1440);
  assert(BTHomeSettings::decode(bytes, enabled, minutes) && !enabled && minutes == 1440);
  bytes[6] ^= 0x80;
  assert(!BTHomeSettings::decode(bytes, enabled, minutes)); // checksum catches corruption
  BTHomeSettings::encode(bytes, true, 0);
  assert(!BTHomeSettings::decode(bytes, enabled, minutes)); // interval validated
  assert(BTHomeSettings::parseMinutes("1", minutes) && minutes == 1);
  assert(BTHomeSettings::parseMinutes("10", minutes) && minutes == 10);
  assert(BTHomeSettings::parseMinutes("1440", minutes) && minutes == 1440);
  assert(!BTHomeSettings::parseMinutes("0", minutes));
  assert(!BTHomeSettings::parseMinutes("1441", minutes));
  assert(!BTHomeSettings::parseMinutes("-1", minutes));
  assert(!BTHomeSettings::parseMinutes("12abc", minutes));
  assert(!BTHomeSettings::parseMinutes("999999999999999999999", minutes));
  assert(!BTHomeSettings::parseMinutes("", minutes));
  std::cout << "PASS: persistent settings, checksums, valid intervals\n";
}
