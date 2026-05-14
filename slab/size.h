#ifndef SLABSTORE_SIZE_CLASS_H
#define SLABSTORE_SIZE_CLASS_H

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <limits>

#include "const.h"
#include "util.h"

namespace SlabStore {

using util::index_most1;
using util::branch_unlikely;

/**
 * @brief mutual conversion between allocation size and cache bin index
 * */
class SizeClass {
  static constexpr size_t kQuantumSize = SlabConst::kQuantumSize;
  static constexpr size_t kMinGapSize = SlabConst::kMinGapSize;
  static constexpr size_t kMaxMediumSize = SlabConst::kMaxMediumSize;
  static constexpr size_t kMaxLookupSize = SlabConst::kMaxLookupSize;
  static constexpr size_t kNSlabsPerGrp = SlabConst::kNSlabsPerGrp;
  static constexpr size_t kNSlabsCached = SlabConst::kNSlabsCached;
  static constexpr size_t kSz2idxTabSize = kMaxLookupSize / kQuantumSize;
  static constexpr size_t kMinGapShift = 7;

  uint16_t idx2sz_tab_[kNSlabsCached];  // index to slab size table (size = tab[idx] * kQuantumSize)
  uint8_t sz2idx_tab_[kSz2idxTabSize];  // size to index table

  static_assert((0x01ul << kMinGapShift) == kMinGapSize);
  static_assert(std::numeric_limits<uint8_t>::max() >= kNSlabsCached);
  static_assert(std::numeric_limits<uint16_t>::max() >= kMaxMediumSize / kQuantumSize);

 public:
  // these division and multiplication operations will be optimized as shifting operations
  static constexpr size_t size2index_compute(size_t size) {
    if(size <= kMinGapSize) return (size - 1) / kQuantumSize;
    size_t most = index_most1(size - 1), gap = 0x01ul << most;
    size_t fidx = (most - kMinGapShift + 1) * kNSlabsPerGrp;
    size_t sidx = (size - gap - 1) / (gap / kNSlabsPerGrp);
    return fidx + sidx;
  }

  static constexpr size_t index2size_compute(size_t idx) {
    if(idx < kNSlabsPerGrp) return (idx + 1) * kQuantumSize;
    size_t gap = kMinGapSize << (idx / kNSlabsPerGrp - 1);
    size_t space = gap / kNSlabsPerGrp, sid = idx % kNSlabsPerGrp;
    return gap + space * (sid + 1);
  }

 private:
  size_t size2index_lookup(size_t size) {
    assert(size <= kMaxLookupSize);
    return sz2idx_tab_[(size - 1) / kQuantumSize];
  }

  size_t index2size_lookup(size_t idx) { return idx2sz_tab_[idx] * kQuantumSize; }

 public:
  SizeClass() : idx2sz_tab_{}, sz2idx_tab_{} {
    for(size_t i = 0; i < kSz2idxTabSize; i++) {
      sz2idx_tab_[i] = size2index_compute((i + 1) * kQuantumSize - 1);
    }
    for(size_t i = 0; i < kNSlabsCached; i++) {
      idx2sz_tab_[i] = index2size_compute(i) / kQuantumSize;
    }
  }

  ~SizeClass() = default;

  SizeClass(const SizeClass&) = default;

  SizeClass& operator=(const SizeClass&) = default;

  /**
   * @brief convert allocation size to cache bin index
   * */
  size_t size2index(size_t size) {
    assert(size > 0 && size <= kMaxMediumSize);
    if(branch_unlikely(size > kMaxLookupSize)) {
      return size2index_compute(size);
    } else {
      assert(size2index_compute(size) == size2index_lookup(size));
      return size2index_lookup(size);
    }
  }

  /**
   * @brief convert cache bin index to max allocation size of the cache bin
   * */
  size_t index2size(size_t idx) {
    assert(idx < kNSlabsCached);
    assert(index2size_compute(idx) == index2size_lookup(idx));
    assert(size2index(index2size_lookup(idx)) == idx);
    return index2size_lookup(idx);
  }
};

}

#endif //SLABSTORE_SIZE_CLASS_H
