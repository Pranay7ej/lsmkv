// Bump allocator for the memtable: everything is freed at once when the
// memtable goes away, so there's no per-entry free.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace lsmkv {

class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes) {
    if (bytes <= remaining_) {
      char* r = ptr_;
      ptr_ += bytes;
      remaining_ -= bytes;
      return r;
    }
    return AllocateFallback(bytes);
  }

  char* AllocateAligned(size_t bytes) {
    constexpr size_t kAlign = alignof(std::max_align_t);
    size_t mod = reinterpret_cast<uintptr_t>(ptr_) & (kAlign - 1);
    size_t slop = mod ? kAlign - mod : 0;
    if (bytes + slop <= remaining_) {
      char* r = ptr_ + slop;
      ptr_ += bytes + slop;
      remaining_ -= bytes + slop;
      return r;
    }
    return AllocateFallback(bytes);  // fresh blocks from new[] are max-aligned
  }

  size_t MemoryUsage() const { return usage_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes) {
    if (bytes > kBlockSize / 4) return NewBlock(bytes);  // big entry: own block, keep the current one
    ptr_ = NewBlock(kBlockSize);
    remaining_ = kBlockSize;
    char* r = ptr_;
    ptr_ += bytes;
    remaining_ -= bytes;
    return r;
  }

  char* NewBlock(size_t n) {
    blocks_.emplace_back(new char[n]);
    usage_.fetch_add(n + sizeof(char*), std::memory_order_relaxed);
    return blocks_.back().get();
  }

  char* ptr_ = nullptr;
  size_t remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> usage_{0};
};

}  // namespace lsmkv
