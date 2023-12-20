#include "iterators.h"

namespace lsmkv {
namespace {

class MergingIterator : public InternalIterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> c) : children_(std::move(c)) {}

  bool Valid() const override { return current_ != nullptr; }
  std::string_view key() const override { return current_->key(); }
  std::string_view value() const override { return current_->value(); }
  Status status() const override {
    for (const auto& c : children_)
      if (!c->status().ok()) return c->status();
    return Status::OK();
  }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    FindSmallest();
  }
  void Seek(std::string_view target) override {
    for (auto& c : children_) c->Seek(target);
    FindSmallest();
  }
  void Next() override {
    current_->Next();
    FindSmallest();
  }

 private:
  void FindSmallest() {
    current_ = nullptr;
    for (auto& c : children_) {
      if (c->Valid() && (!current_ || CompareInternalKey(c->key(), current_->key()) < 0)) current_ = c.get();
    }
  }

  std::vector<std::unique_ptr<InternalIterator>> children_;
  InternalIterator* current_ = nullptr;
};

}  // namespace

std::unique_ptr<InternalIterator> NewMergingIterator(std::vector<std::unique_ptr<InternalIterator>> children) {
  if (children.size() == 1) return std::move(children[0]);
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace lsmkv
