// Bloom filter over user keys, one per table.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsmkv {

inline uint32_t BloomHash(std::string_view key) {
  // Murmur-ish 32-bit hash; doesn't need to be fancy, just well mixed.
  const uint32_t m = 0xc6a4a793, seed = 0xbc9f1d34;
  uint32_t h = seed ^ uint32_t(key.size() * m);
  size_t i = 0;
  for (; i + 4 <= key.size(); i += 4) {
    uint32_t w = uint32_t(uint8_t(key[i])) | uint32_t(uint8_t(key[i + 1])) << 8 |
                 uint32_t(uint8_t(key[i + 2])) << 16 | uint32_t(uint8_t(key[i + 3])) << 24;
    h += w;
    h *= m;
    h ^= h >> 16;
  }
  switch (key.size() - i) {
    case 3: h += uint32_t(uint8_t(key[i + 2])) << 16; [[fallthrough]];
    case 2: h += uint32_t(uint8_t(key[i + 1])) << 8; [[fallthrough]];
    case 1:
      h += uint32_t(uint8_t(key[i]));
      h *= m;
      h ^= h >> 24;
  }
  return h;
}

class BloomBuilder {
 public:
  explicit BloomBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {}
  void AddKey(std::string_view user_key) {
    uint32_t h = BloomHash(user_key);
    if (hashes_.empty() || hashes_.back() != h) hashes_.push_back(h);  // consecutive dup user keys
  }
  // Serialized filter: bit array followed by one byte holding k.
  std::string Finish() const {
    size_t bits = std::max<size_t>(64, hashes_.size() * size_t(bits_per_key_));
    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;
    int k = int(bits_per_key_ * 0.69);  // ln 2 * bits/key minimises false positives
    k = k < 1 ? 1 : (k > 30 ? 30 : k);
    std::string out(bytes, '\0');
    for (uint32_t h : hashes_) {
      const uint32_t delta = (h >> 17) | (h << 15);  // double hashing
      for (int j = 0; j < k; ++j) {
        const uint32_t pos = h % bits;
        out[pos / 8] = char(out[pos / 8] | (1 << (pos % 8)));
        h += delta;
      }
    }
    out.push_back(char(k));
    return out;
  }

 private:
  int bits_per_key_;
  std::vector<uint32_t> hashes_;
};

// False means the key is definitely not there.
inline bool BloomMayContain(std::string_view filter, std::string_view user_key) {
  if (filter.size() < 2) return true;
  const size_t bits = (filter.size() - 1) * 8;
  const int k = uint8_t(filter.back());
  if (k > 30) return true;  // reserved for future encodings
  uint32_t h = BloomHash(user_key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (int j = 0; j < k; ++j) {
    const uint32_t pos = h % bits;
    if (!(uint8_t(filter[pos / 8]) & (1 << (pos % 8)))) return false;
    h += delta;
  }
  return true;
}

}  // namespace lsmkv
