// Internal keys: user_key + 8 byte tag, tag = (sequence << 8) | type.
// Ordered by user key ascending, then sequence descending, so the newest
// version of a key comes first.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "coding.h"

namespace lsmkv {

using SequenceNumber = uint64_t;
constexpr SequenceNumber kMaxSequenceNumber = (uint64_t{1} << 56) - 1;

enum ValueType : uint8_t { kTypeDeletion = 0, kTypeValue = 1 };
// Seeking uses the highest type so a lookup key sorts before every entry
// with the same user key and sequence.
constexpr ValueType kValueTypeForSeek = kTypeValue;

inline uint64_t PackTag(SequenceNumber seq, ValueType t) { return (seq << 8) | t; }

inline void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber seq,
                              ValueType t) {
  dst->append(user_key.data(), user_key.size());
  PutFixed64(dst, PackTag(seq, t));
}

inline std::string_view ExtractUserKey(std::string_view ikey) { return ikey.substr(0, ikey.size() - 8); }
inline uint64_t ExtractTag(std::string_view ikey) { return DecodeFixed64(ikey.data() + ikey.size() - 8); }
inline SequenceNumber ExtractSeq(std::string_view ikey) { return ExtractTag(ikey) >> 8; }
inline ValueType ExtractType(std::string_view ikey) {
  return static_cast<ValueType>(ExtractTag(ikey) & 0xff);
}

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;
};

inline bool ParseInternalKey(std::string_view ikey, ParsedInternalKey* out) {
  if (ikey.size() < 8) return false;
  uint64_t tag = ExtractTag(ikey);
  uint8_t t = tag & 0xff;
  if (t > kTypeValue) return false;
  out->user_key = ExtractUserKey(ikey);
  out->sequence = tag >> 8;
  out->type = static_cast<ValueType>(t);
  return true;
}

inline int CompareInternalKey(std::string_view a, std::string_view b) {
  int r = ExtractUserKey(a).compare(ExtractUserKey(b));
  if (r != 0) return r;
  uint64_t ta = ExtractTag(a), tb = ExtractTag(b);
  if (ta > tb) return -1;  // bigger sequence first
  if (ta < tb) return 1;
  return 0;
}

struct InternalKeyLess {
  bool operator()(std::string_view a, std::string_view b) const { return CompareInternalKey(a, b) < 0; }
};

// Key used for point lookups at a snapshot.
inline std::string LookupKey(std::string_view user_key, SequenceNumber seq) {
  std::string k;
  AppendInternalKey(&k, user_key, seq, kValueTypeForSeek);
  return k;
}

// File names inside the DB directory.
inline std::string TableFileName(const std::string& dir, uint64_t n) {
  char buf[32];
  snprintf(buf, sizeof buf, "/%06llu.sst", static_cast<unsigned long long>(n));
  return dir + buf;
}
inline std::string LogFileName(const std::string& dir, uint64_t n) {
  char buf[32];
  snprintf(buf, sizeof buf, "/%06llu.log", static_cast<unsigned long long>(n));
  return dir + buf;
}
inline std::string ManifestFileName(const std::string& dir, uint64_t n) {
  char buf[32];
  snprintf(buf, sizeof buf, "/MANIFEST-%06llu", static_cast<unsigned long long>(n));
  return dir + buf;
}
inline std::string CurrentFileName(const std::string& dir) { return dir + "/CURRENT"; }
inline std::string LockFileName(const std::string& dir) { return dir + "/LOCK"; }

}  // namespace lsmkv
