#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include "../../examples/simple_repeater/BTHomePayload.h"

int main() {
  uint8_t data[BTHomePayload::kMaxLength]{};
  const size_t n = BTHomePayload::encode(data, 7, 3950, true, -525);
  const uint8_t expected[] = {0xD2, 0xFC, 0x44, 0x00, 0x07, 0x02, 0xF3,
                               0xFD, 0x0C, 0x6E, 0x0F};
  assert(n == sizeof(expected));
  assert(memcmp(data, expected, n) == 0);

  uint8_t absent[BTHomePayload::kMaxLength]{};
  const size_t m = BTHomePayload::encode(absent, 8, 4200, false, 0);
  const uint8_t expectedAbsent[] = {0xD2, 0xFC, 0x44, 0x00, 0x08, 0x0C, 0x68, 0x10};
  assert(m == sizeof(expectedAbsent));
  assert(memcmp(absent, expectedAbsent, m) == 0);
  std::cout << "PASS: BTHome v2 packet ID / temperature / voltage encoding\n";
}
