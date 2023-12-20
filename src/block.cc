#include "block.h"

#include <algorithm>

#include "coding.h"
#include "dbformat.h"

namespace lsmkv {

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  size_t shared = 0;
  if (counter_ < interval_) {
    const size_t n = std::min(last_key_.size(), key.size());
    while (shared < n && last_key_[shared] == key[shared]) ++shared;
  } else {
    restarts_.push_back(static_cast<uint32_t>(buf_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;
  PutVarint32(&buf_, static_cast<uint32_t>(shared));
  PutVarint32(&buf_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buf_, static_cast<uint32_t>(value.size()));
  buf_.append(key.data() + shared, non_shared);
  buf_.append(value.data(), value.size());
  last_key_.assign(key.data(), key.size());
  ++counter_;
}

std::string_view BlockBuilder::Finish() {
  for (uint32_t r : restarts_) PutFixed32(&buf_, r);
  PutFixed32(&buf_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buf_;
}

void BlockBuilder::Reset() {
  buf_.clear();
  restarts_.assign(1, 0);
  counter_ = 0;
  last_key_.clear();
  finished_ = false;
}

namespace {

class BlockIter : public InternalIterator {
 public:
  explicit BlockIter(BlockContents c) : contents_(std::move(c)) {
    const std::string& d = *contents_;
    if (d.size() < 4) {
      Corrupt();
      return;
    }
    num_restarts_ = DecodeFixed32(d.data() + d.size() - 4);
    if (num_restarts_ == 0 || uint64_t(num_restarts_) * 4 + 4 > d.size()) {
      Corrupt();
      return;
    }
    restarts_off_ = d.size() - 4 - num_restarts_ * 4;
    current_ = restarts_off_;
  }

  bool Valid() const override { return current_ < restarts_off_; }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }
  Status status() const override { return status_; }

  void SeekToFirst() override {
    if (!status_.ok()) return;
    JumpToRestart(0);
    ParseNext();
  }

  void Next() override { ParseNext(); }

  void Seek(std::string_view target) override {
    if (!status_.ok()) return;
    // Last restart whose key is < target.
    uint32_t lo = 0, hi = num_restarts_ - 1;
    while (lo < hi) {
      uint32_t mid = (lo + hi + 1) / 2;
      std::string_view k;
      if (!RestartKey(mid, &k)) return;
      if (CompareInternalKey(k, target) < 0)
        lo = mid;
      else
        hi = mid - 1;
    }
    JumpToRestart(lo);
    while (ParseNext() && CompareInternalKey(key_, target) < 0) {
    }
  }

 private:
  void Corrupt() {
    status_ = Status::Corruption("bad block");
    current_ = restarts_off_ = 0;
    key_.clear();
    value_ = {};
  }

  uint32_t RestartOffset(uint32_t i) const {
    return DecodeFixed32(contents_->data() + restarts_off_ + i * 4);
  }

  bool RestartKey(uint32_t i, std::string_view* k) {
    const char* p = contents_->data() + RestartOffset(i);
    const char* limit = contents_->data() + restarts_off_;
    uint32_t shared, non_shared, vlen;
    if (!(p = GetVarint32Ptr(p, limit, &shared)) || !(p = GetVarint32Ptr(p, limit, &non_shared)) ||
        !(p = GetVarint32Ptr(p, limit, &vlen)) || shared != 0 || p + non_shared > limit) {
      Corrupt();
      return false;
    }
    *k = std::string_view(p, non_shared);
    return true;
  }

  void JumpToRestart(uint32_t i) {
    key_.clear();
    next_ = RestartOffset(i);
    current_ = next_;
  }

  bool ParseNext() {
    current_ = next_;
    if (current_ >= restarts_off_) {
      current_ = restarts_off_;
      return false;
    }
    const char* base = contents_->data();
    const char* p = base + current_;
    const char* limit = base + restarts_off_;
    uint32_t shared, non_shared, vlen;
    if (!(p = GetVarint32Ptr(p, limit, &shared)) || !(p = GetVarint32Ptr(p, limit, &non_shared)) ||
        !(p = GetVarint32Ptr(p, limit, &vlen)) || shared > key_.size() ||
        p + non_shared + vlen > limit) {
      Corrupt();
      return false;
    }
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = std::string_view(p + non_shared, vlen);
    next_ = static_cast<size_t>(p + non_shared + vlen - base);
    return true;
  }

  BlockContents contents_;
  uint32_t num_restarts_ = 0;
  size_t restarts_off_ = 0;
  size_t current_ = 0;  // offset of the current entry
  size_t next_ = 0;
  std::string key_;
  std::string_view value_;
  Status status_;
};

}  // namespace

std::unique_ptr<InternalIterator> NewBlockIterator(BlockContents contents) {
  return std::make_unique<BlockIter>(std::move(contents));
}

}  // namespace lsmkv
