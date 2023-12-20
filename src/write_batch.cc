#include "write_batch.h"

namespace lsmkv {

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() { rep_.assign(kBatchHeader, '\0'); }

size_t WriteBatch::Count() const { return WriteBatchInternal::Count(*this); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  EncodeFixed32(rep_.data() + 8, WriteBatchInternal::Count(*this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  EncodeFixed32(rep_.data() + 8, WriteBatchInternal::Count(*this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixed(&rep_, key);
}

}  // namespace lsmkv
