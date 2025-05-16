/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_TCACHE_H
#define SLABSTORE_TCACHE_H

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <deque>
#include <cstdio>

#include "const.h"
#include "arena.h"
#include "token.h"
#include "size.h"
#include "desc.h"
#include "util.h"

namespace SlabStore {

using util::rounddown;

/**
 * @brief thread local cache bin for a specific slab size class
 * */
class CacheBin {
  ExtentCase* ext_case_;  // the global extent case
  Arena* arenas_;         // the global arenas array
  Arena* arena_;          // corresponding thread cache
  uint32_t index_;        // the index of CacheBin / slab size class
  uint32_t capacity_;     // the capacity of current cache bin
  uint32_t stock_;        // stock & restock quantity of cache bin
  std::deque<region_t> regions_; // cached regions

  static constexpr size_t kNSlabsCached = SlabConst::kNSlabsCached;
  static constexpr size_t kExtentSize = SlabConst::kExtentSize;
  static constexpr size_t kMaxSmallSize = SlabConst::kMaxSmallSize;
  static constexpr size_t kMaxMediumSize = SlabConst::kMaxMediumSize;

 private:
  /**
   * @brief release the first region back to its corresponding arena
   * */
  void spill_front() {
    auto [token, ptr] = regions_.front();
    regions_.pop_front();
    ExtentDesc* desc = ext_case_->descriptor(ptr);
    assert(desc->type() == kSmall || desc->type() == kMedium);
    uintptr_t ext = rounddown((uintptr_t) ptr, kExtentSize);
    size_t rsize = desc->size(), rid = ((uintptr_t) ptr - ext) / rsize;

    if(desc->type() == kSmall) {
      auto meta = (SmallMeta*) ((uintptr_t) ext + rsize * rid);
      size_t count = meta->count(), size = meta->size();
      assert(size <= kMaxSmallSize);
      uintptr_t start = (uintptr_t) meta + rsize - count * size;
      size_t index = ((uintptr_t) ptr - start) / size;
      assert(index * size + start == (uintptr_t) ptr);
      arenas_[meta->arena()].release(meta, meta, index);
    } else {
      auto meta = (MediumMeta*) desc->runs() + rid;
      size_t size = meta->size();
      assert(size > kMaxSmallSize && size <= kMaxMediumSize);
      uintptr_t start = (uintptr_t) ext + rsize * rid;
      size_t index = ((uintptr_t) ptr - start) / size;
      assert(index * size + start == (uintptr_t) ptr);
      arenas_[meta->arena()].release(meta, (void*) start, index);
    }
  }

  /**
   * @brief drain excess regions to their corresponding arena
   * */
  void spill_cache_bin() {
    if(regions_.size() >= capacity_) {
      while(regions_.size() >= stock_) {
        spill_front();
      }
    }
  }

 public:
  CacheBin() : ext_case_(nullptr), arenas_(nullptr), arena_(nullptr),
               index_(-1), capacity_(-1), stock_(-1), regions_() {}

  ~CacheBin() { while(!regions_.empty()) { spill_front(); } }

  CacheBin(const CacheBin&) = delete;

  CacheBin& operator=(const CacheBin&) = delete;

  /**
   * @brief CacheBin initialization
   * @param ext_case the global extent case
   * @param arenas the global arenas array
   * @param arena corresponding arena
   * @param index the index of CacheBin / slab size class
   * @param capacity the capacity of current cache bin
   * @param stock the stock & restock quantity of cache bin
   * */
  void init(ExtentCase* ext_case, Arena* arenas, Arena* arena, size_t index, size_t capacity, size_t stock) {
    ext_case_ = ext_case, arenas_ = arenas, arena_ = arena, index_ = index, capacity_ = capacity, stock_ = stock;
    assert(ext_case != nullptr && arenas != nullptr && arena != nullptr && index < kNSlabsCached & stock <= capacity);
  }

  /**
   * @brief allocate a reserved region from current cache bin,
   * if current cache bin is empty, restock regions from arena
   * */
  region_t acquire() {
    if(regions_.empty()) {
      // has no cached regions, restock from arena
      arena_->fill_cache_bin(regions_, stock_, index_);
      // failed to restock for some reason, e.g. no more space
      if(regions_.empty()) { return {nullptr, nullptr}; }
    }
    // todo: adjust allocation order, to avoid cache line flush at the same line
    auto ret = regions_.front();
    regions_.pop_front();
    return ret;
  }

  /**
   * @brief release a region back to thread local cache bin
   * */
  void release(region_t obj) {
    assert(!obj.first->busy());
    regions_.push_back(obj);
    // excess regions are drained to arena
    spill_cache_bin();
  }
};


/**
 * @brief Thread Cache for small & medium size class, but actually deal
 * with all allocation request, small & medium size allocation request
 * goes to CacheBin for a pre-reserved memory block, large allocation
 * request goes to arena
 * */
class ThreadCache {
  static constexpr size_t kMaxSmallSize = SlabConst::kMaxSmallSize;
  static constexpr size_t kMaxMediumSize = SlabConst::kMaxMediumSize;
  static constexpr size_t kMaxLargeSize = SlabConst::kMaxLargeSize;
  static constexpr size_t kNSlabsPerGrp = SlabConst::kNSlabsPerGrp;
  static constexpr size_t kNSlabsCached = SlabConst::kNSlabsCached;
  static constexpr size_t kExtentSize = SlabConst::kExtentSize;

  ExtentCase* ext_case_;  // the global extent case for release operation to determine the size of regions
  SizeClass* sc_;         // for computing index of CacheBin for small & medium allocation
  RunCase* run_cases_;    // the global RunCase array
  Arena* arena_;          // corresponding arena
  CacheBin bins_[kNSlabsCached]; // cache bins for small and medium slab size class

  static_assert(kNSlabsCached == SizeClass::size2index_compute(kMaxMediumSize) + 1);

 private:
  /**
   * @brief release a large region
   * @param desc the corresponding extent descriptor
   * @param prt the start address of the region
   * */
  void large_release(ExtentDesc* desc, void* ptr) {
    assert(desc->type() == kLarge && (uintptr_t) ptr % desc->size() == 0);
    run_cases_[desc->rcase()].large_release(desc, ptr);
  }

  /**
   * @brief release a medium region
   * @param desc the corresponding extent descriptor
   * @param prt the start address of the region
   * */
  void medium_release(ExtentDesc* desc, void* ptr) {
    assert(desc->type() == kMedium);
    uintptr_t ext = rounddown((uintptr_t) ptr, kExtentSize);
    size_t rsize = desc->size(), rid = ((uintptr_t) ptr - ext) / rsize;
    auto meta = (MediumMeta*) desc->runs() + rid;
    size_t size = meta->size();
    assert(size > kMaxSmallSize && size <= kMaxMediumSize);
    uintptr_t start = (uintptr_t) ext + rsize * rid;
    size_t index = ((uintptr_t) ptr - start) / size;
    assert(index * size + start == (uintptr_t) ptr);
    token_t* token = meta->token(index);
    token->fire(); // persistently mark this region as free
    size_t bin_idx = sc_->size2index(size);
    bins_[bin_idx].release({token, ptr});
  }

  /**
   * @brief release a small region
   * @param desc the corresponding extent descriptor
   * @param prt the start address of the region
   * */
  void small_release(ExtentDesc* desc, void* ptr) {
    assert(desc->type() == kSmall);
    uintptr_t ext = rounddown((uintptr_t) ptr, kExtentSize);
    size_t rsize = desc->size(), rid = ((uintptr_t) ptr - ext) / rsize;
    auto meta = (SmallMeta*) ((uintptr_t) ext + rsize * rid);
    size_t count = meta->count(), size = meta->size();
    assert(size <= kMaxSmallSize);
    uintptr_t start = (uintptr_t) meta + rsize - count * size;
    size_t index = ((uintptr_t) ptr - start) / size;
    assert(index * size + start == (uintptr_t) ptr);
    token_t* token = meta->token(index);
    token->fire(); // persistently mark this region as free
    size_t bin_idx = sc_->size2index(size);
    bins_[bin_idx].release({token, ptr});
  }

 public:
  ThreadCache(ExtentCase* ext_case, SizeClass* sc, RunCase* run_cases, Arena* arenas, Arena* arena) :
    ext_case_(ext_case), sc_(sc), run_cases_(run_cases), arena_(arena), bins_{} {
    assert(arena != nullptr && ext_case != nullptr && sc != nullptr);
    assert(run_cases != nullptr && arenas != nullptr);
    arena_->nbinds()++;  // bind current thread cache to its corresponding arena
    for(size_t bid = 0; bid < kNSlabsCached; bid++) {
      size_t capacity = SlabConst::kCacheBinCapTab[bid / kNSlabsPerGrp];
      size_t stock = SlabConst::kCacheBinStockTab[bid / kNSlabsPerGrp];
      bins_[bid].init(ext_case, arenas, arena, bid, capacity, stock);
    }
  }

  /**
   * @brief return all free regions to the corresponding arena
   * */
  ~ThreadCache() {
    arena_->nbinds()--; // unbind
  }

  ThreadCache(const ThreadCache&) = delete;

  ThreadCache& operator=(const ThreadCache&) = delete;

  /**
   * @brief allocate a region
   * @param size region size
   * @return the start address of the region and its token
   * */
  region_t acquire(size_t size) {
    if(size <= kMaxMediumSize) {
      if(size == 0) return {nullptr, nullptr};
      // small or medium region size, allocate a region from thread cache
      size_t bin_idx = sc_->size2index(size);
      assert(bin_idx < kNSlabsCached);
      return bins_[bin_idx].acquire();
    } else if(size <= kMaxLargeSize) {
      return arena_->large_acquire(size);
    }
    // region size larger than kMaxLargeSize is not supported
    return {nullptr, nullptr};
  }

  /**
   * @brief free a region
   * @param ptr the start address of the region
   * */
  void release(void* ptr) {
    if(ptr == nullptr) return;
    ExtentDesc* desc = ext_case_->descriptor(ptr);
    switch(desc->type()) {
      case kLarge:
        return large_release(desc, ptr);
      case kMedium:
        return medium_release(desc, ptr);
      case kSmall:
        return small_release(desc, ptr);
      default:
        fprintf(stderr, "[ERROR]: release, unknown type\n");
        exit(EXIT_FAILURE);
    }
  }
};

}

#endif //SLABSTORE_TCACHE_H
