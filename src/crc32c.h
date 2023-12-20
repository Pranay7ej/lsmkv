// CRC-32C (Castagnoli), table driven. Plenty fast next to disk I/O.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lsmkv::crc32c {

namespace detail {
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    t[i] = c;
  }
  return t;
}
inline constexpr std::array<uint32_t, 256> kTable = MakeTable();
}  // namespace detail

inline uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; ++i)
    crc = detail::kTable[(crc ^ static_cast<unsigned char>(data[i])) & 0xff] ^ (crc >> 8);
  return ~crc;
}

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// Stored CRCs are masked so that a CRC of data that itself contains CRCs
// doesn't come out as something trivially predictable.
inline uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + 0xa282ead8u; }
inline uint32_t Unmask(uint32_t m) {
  uint32_t rot = m - 0xa282ead8u;
  return (rot >> 17) | (rot << 15);
}

}  // namespace lsmkv::crc32c
