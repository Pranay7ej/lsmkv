// LRU cache charged by bytes. Values are shared_ptrs, so an evicted entry
// stays alive for any iterator still holding it.
#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lsmkv {

template <typename V>
class LruCache {
 public:
  explicit LruCache(size_t capacity) : capacity_(capacity) {}

  std::shared_ptr<V> Lookup(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) {
      ++misses_;
      return nullptr;
    }
    ++hits_;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->value;
  }

  void Insert(const std::string& key, std::shared_ptr<V> value, size_t charge) {
    std::lock_guard<std::mutex> lk(mu_);
    if (charge > capacity_) return;  // wouldn't fit; don't flush everything for it
    auto it = map_.find(key);
    if (it != map_.end()) {
      usage_ -= it->second->charge;
      lru_.erase(it->second);
      map_.erase(it);
    }
    lru_.push_front(Entry{key, std::move(value), charge});
    map_[key] = lru_.begin();
    usage_ += charge;
    while (usage_ > capacity_ && !lru_.empty()) {
      usage_ -= lru_.back().charge;
      map_.erase(lru_.back().key);
      lru_.pop_back();
    }
  }

  void Erase(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return;
    usage_ -= it->second->charge;
    lru_.erase(it->second);
    map_.erase(it);
  }

  size_t usage() const {
    std::lock_guard<std::mutex> lk(mu_);
    return usage_;
  }
  uint64_t hits() const {
    std::lock_guard<std::mutex> lk(mu_);
    return hits_;
  }
  uint64_t misses() const {
    std::lock_guard<std::mutex> lk(mu_);
    return misses_;
  }

 private:
  struct Entry {
    std::string key;
    std::shared_ptr<V> value;
    size_t charge;
  };
  mutable std::mutex mu_;
  size_t capacity_;
  size_t usage_ = 0;
  uint64_t hits_ = 0, misses_ = 0;
  std::list<Entry> lru_;
  std::unordered_map<std::string, typename std::list<Entry>::iterator> map_;
};

}  // namespace lsmkv
