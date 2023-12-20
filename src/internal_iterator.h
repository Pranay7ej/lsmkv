// Iterator over internal keys (user key + tag). Everything below the DB
// (memtables, blocks, tables, levels, merging) speaks this interface.
#pragma once

#include <string_view>

#include "lsmkv/status.h"

namespace lsmkv {

class InternalIterator {
 public:
  virtual ~InternalIterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view internal_key) = 0;
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  virtual Status status() const { return Status::OK(); }
};

class EmptyIterator : public InternalIterator {
 public:
  explicit EmptyIterator(Status s = Status::OK()) : s_(std::move(s)) {}
  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view) override {}
  void Next() override {}
  std::string_view key() const override { return {}; }
  std::string_view value() const override { return {}; }
  Status status() const override { return s_; }

 private:
  Status s_;
};

}  // namespace lsmkv
