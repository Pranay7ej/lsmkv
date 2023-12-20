// Little-endian fixed ints and varints.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lsmkv {

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  for (int i = 0; i < 4; ++i) buf[i] = static_cast<char>(v >> (8 * i));
  dst->append(buf, 4);
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; ++i) buf[i] = static_cast<char>(v >> (8 * i));
  dst->append(buf, 8);
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= uint32_t(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

inline void EncodeFixed32(char* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<char>(v >> (8 * i));
}

inline void EncodeFixed64(char* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<char>(v >> (8 * i));
}

inline void PutVarint64(std::string* dst, uint64_t v) {
  while (v >= 0x80) {
    dst->push_back(static_cast<char>(v | 0x80));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

inline void PutVarint32(std::string* dst, uint32_t v) { PutVarint64(dst, v); }

inline int VarintLength(uint64_t v) {
  int n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

// Returns the byte after the varint, or nullptr if it's truncated/too long.
inline const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* v) {
  uint64_t result = 0;
  for (int shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = static_cast<unsigned char>(*p++);
    result |= (byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      *v = result;
      return p;
    }
  }
  return nullptr;
}

inline const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* v) {
  uint64_t v64;
  const char* q = GetVarint64Ptr(p, limit, &v64);
  if (!q || v64 > 0xffffffffu) return nullptr;
  *v = static_cast<uint32_t>(v64);
  return q;
}

// Consumes a varint from the front of *in.
inline bool GetVarint64(std::string_view* in, uint64_t* v) {
  const char* p = GetVarint64Ptr(in->data(), in->data() + in->size(), v);
  if (!p) return false;
  in->remove_prefix(size_t(p - in->data()));
  return true;
}

inline bool GetVarint32(std::string_view* in, uint32_t* v) {
  uint64_t v64;
  if (!GetVarint64(in, &v64) || v64 > 0xffffffffu) return false;
  *v = static_cast<uint32_t>(v64);
  return true;
}

inline void PutLengthPrefixed(std::string* dst, std::string_view s) {
  PutVarint32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s.data(), s.size());
}

inline bool GetLengthPrefixed(std::string_view* in, std::string_view* out) {
  uint32_t len;
  if (!GetVarint32(in, &len) || in->size() < len) return false;
  *out = in->substr(0, len);
  in->remove_prefix(len);
  return true;
}

}  // namespace lsmkv
