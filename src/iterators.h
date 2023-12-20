#pragma once

#include <memory>
#include <vector>

#include "dbformat.h"
#include "internal_iterator.h"

namespace lsmkv {

// Merges sorted children into one sorted stream. Children are few (memtables,
// L0 files, one per deeper level), so a linear scan for the smallest is fine.
std::unique_ptr<InternalIterator> NewMergingIterator(std::vector<std::unique_ptr<InternalIterator>> children);

}  // namespace lsmkv
