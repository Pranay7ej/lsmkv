// WriteBatch layout: sequence u64 | count u32 | records
//   record := kTypeValue key(len-prefixed) value(len-prefixed)
//           | kTypeDeletion key(len-prefixed)
// The whole rep is what goes into the WAL, so a batch is one WAL record and
// replays all-or-nothing.
#pragma once

#include "dbformat.h"
#include "lsmkv/db.h"
#include "memtable.h"

namespace lsmkv {

constexpr size_t kBatchHeader = 12;

class WriteBatchInternal {
 public:
  static SequenceNumber Sequence(const WriteBatch& b) { return DecodeFixed64(b.rep_.data()); }
  static void SetSequence(WriteBatch* b, SequenceNumber s) { EncodeFixed64(b->rep_.data(), s); }
  static uint32_t Count(const WriteBatch& b) { return DecodeFixed32(b.rep_.data() + 8); }
  static std::string_view Contents(const WriteBatch& b) { return b.rep_; }
  static Status SetContents(WriteBatch* b, std::string_view contents) {
    if (contents.size() < kBatchHeader) return Status::Corruption("batch too small");
    b->rep_.assign(contents.data(), contents.size());
    return Status::OK();
  }

  // Calls put(key, value) / del(key) for each record, in order.
  template <typename Put, typename Del>
  static Status Iterate(const WriteBatch& b, Put&& put, Del&& del) {
    std::string_view in(b.rep_);
    in.remove_prefix(kBatchHeader);
    uint32_t found = 0;
    while (!in.empty()) {
      const char tag = in[0];
      in.remove_prefix(1);
      std::string_view k, v;
      if (tag == kTypeValue) {
        if (!GetLengthPrefixed(&in, &k) || !GetLengthPrefixed(&in, &v)) return Status::Corruption("bad Put");
        put(k, v);
      } else if (tag == kTypeDeletion) {
        if (!GetLengthPrefixed(&in, &k)) return Status::Corruption("bad Delete");
        del(k);
      } else {
        return Status::Corruption("unknown batch tag");
      }
      ++found;
    }
    if (found != Count(b)) return Status::Corruption("batch count mismatch");
    return Status::OK();
  }

  static Status InsertInto(const WriteBatch& b, MemTable* mem) {
    SequenceNumber seq = Sequence(b);
    return Iterate(
        b, [&](std::string_view k, std::string_view v) { mem->Add(seq++, kTypeValue, k, v); },
        [&](std::string_view k) { mem->Add(seq++, kTypeDeletion, k, {}); });
  }
};

}  // namespace lsmkv
