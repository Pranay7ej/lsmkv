#include "memtable.h"

#include <cstring>

namespace lsmkv {
namespace {

std::string_view GetLengthPrefixedPtr(const char* p) {
  uint32_t len = 0;
  p = GetVarint32Ptr(p, p + 5, &len);  // entries are well-formed; 5 bytes max for a varint32
  return std::string_view(p, len);
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return CompareInternalKey(GetLengthPrefixedPtr(a), GetLengthPrefixedPtr(b));
}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value) {
  const size_t ikey_len = key.size() + 8;
  const size_t len = VarintLength(ikey_len) + ikey_len + VarintLength(value.size()) + value.size();
  std::string tmp;
  tmp.reserve(len);
  PutVarint32(&tmp, static_cast<uint32_t>(ikey_len));
  AppendInternalKey(&tmp, key, seq, type);
  PutVarint32(&tmp, static_cast<uint32_t>(value.size()));
  tmp.append(value.data(), value.size());
  char* buf = arena_.Allocate(len);
  std::memcpy(buf, tmp.data(), len);
  table_.Insert(buf);
  ++entries_;
}

bool MemTable::Get(std::string_view user_key, SequenceNumber seq, std::string* value, bool* deleted) const {
  std::string lookup;
  PutVarint32(&lookup, static_cast<uint32_t>(user_key.size() + 8));
  AppendInternalKey(&lookup, user_key, seq, kValueTypeForSeek);

  Table::Iterator it(&table_);
  it.Seek(lookup.data());
  if (!it.Valid()) return false;
  // Seek landed on the newest entry with sequence <= seq for this key, if any.
  const char* entry = it.key();
  std::string_view ikey = GetLengthPrefixedPtr(entry);
  if (ExtractUserKey(ikey) != user_key) return false;
  if (ExtractType(ikey) == kTypeDeletion) {
    *deleted = true;
    return true;
  }
  std::string_view v = GetLengthPrefixedPtr(ikey.data() + ikey.size());
  value->assign(v.data(), v.size());
  *deleted = false;
  return true;
}

class MemTable::Iter : public InternalIterator {
 public:
  explicit Iter(const Table* t) : it_(t) {}
  bool Valid() const override { return it_.Valid(); }
  void SeekToFirst() override { it_.SeekToFirst(); }
  void Seek(std::string_view ikey) override {
    scratch_.clear();
    PutLengthPrefixed(&scratch_, ikey);
    it_.Seek(scratch_.data());
  }
  void Next() override { it_.Next(); }
  std::string_view key() const override { return GetLengthPrefixedPtr(it_.key()); }
  std::string_view value() const override {
    std::string_view k = key();
    return GetLengthPrefixedPtr(k.data() + k.size());
  }

 private:
  Table::Iterator it_;
  std::string scratch_;
};

std::unique_ptr<InternalIterator> MemTable::NewIterator() const { return std::make_unique<Iter>(&table_); }

}  // namespace lsmkv
