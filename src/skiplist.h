// Skip list with one writer and any number of lock-free readers.
//
// Writers must be serialized externally (the DB mutex). Readers need no lock:
// a node is fully built before it's published with a release store, and
// readers follow links with acquire loads, so they only ever see complete
// nodes. Nodes are never removed while the list is alive.
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <random>

#include "arena.h"

namespace lsmkv {

template <typename Key, class Comparator>
class SkipList {
  struct Node;

 public:
  SkipList(Comparator cmp, Arena* arena)
      : cmp_(cmp), arena_(arena), head_(NewNode(Key(), kMaxHeight)), rng_(0xdeadbeef) {
    for (int i = 0; i < kMaxHeight; ++i) head_->SetNext(i, nullptr);
  }

  // Requires: nothing equal to key is in the list.
  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);
    assert(x == nullptr || !Equal(key, x->key));
    (void)x;

    const int height = RandomHeight();
    const int cur = max_height_.load(std::memory_order_relaxed);
    if (height > cur) {
      for (int i = cur; i < height; ++i) prev[i] = head_;
      // Readers that see the new height before the node is linked just
      // find nullptr at the top levels of head_ and drop down. Harmless.
      max_height_.store(height, std::memory_order_relaxed);
    }

    x = NewNode(key, height);
    for (int i = 0; i < height; ++i) {
      x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
      prev[i]->SetNext(i, x);  // publish
    }
  }

  bool Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return x != nullptr && Equal(key, x->key);
  }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}
    bool Valid() const { return node_ != nullptr; }
    const Key& key() const { return node_->key; }
    void Next() { node_ = node_->Next(0); }
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  static constexpr int kMaxHeight = 12;

  struct Node {
    explicit Node(const Key& k) : key(k) {}
    Key const key;
    Node* Next(int n) { return next_[n].load(std::memory_order_acquire); }
    void SetNext(int n, Node* x) { next_[n].store(x, std::memory_order_release); }
    Node* NoBarrierNext(int n) { return next_[n].load(std::memory_order_relaxed); }
    void NoBarrierSetNext(int n, Node* x) { next_[n].store(x, std::memory_order_relaxed); }

   private:
    std::atomic<Node*> next_[1];  // really `height` entries, allocated past the end
  };

  Node* NewNode(const Key& key, int height) {
    char* mem = arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
    return new (mem) Node(key);
  }

  int RandomHeight() {
    int h = 1;
    while (h < kMaxHeight && (rng_() & 3) == 0) ++h;  // p = 1/4
    return h;
  }

  bool Equal(const Key& a, const Key& b) const { return cmp_(a, b) == 0; }

  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = max_height_.load(std::memory_order_relaxed) - 1;
    for (;;) {
      Node* next = x->Next(level);
      if (next != nullptr && cmp_(next->key, key) < 0) {
        x = next;
      } else {
        if (prev) prev[level] = x;
        if (level == 0) return next;
        --level;
      }
    }
  }

  Comparator const cmp_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_{1};
  std::minstd_rand rng_;
};

}  // namespace lsmkv
