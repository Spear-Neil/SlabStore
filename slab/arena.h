/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_ARENA_H
#define SLABSTORE_ARENA_H

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <atomic>
#include <deque>
#include <unordered_map>

#include "const.h"
#include "extent.h"
#include "run.h"
#include "bits.h"
#include "size.h"
#include "desc.h"
#include "util.h"


namespace SlabStore {

using util::MutexLock;
using util::LockGuard;

/**
 * @brief in-memory map structure for managing allocation status of regions
 * in runs, all non-full and non-empty runs reside in ArenaBins
 * */
class ArenaBin {
  MutexLock<> lock_;   // region (de-)allocation needs to hold this lock
  uint32_t arena_;     // the index of corresponding arena in the global arena array
  uint32_t index_;     // the ArenaBin index within bins array, corresponding to size class index
  RegionType type_;    // type of regions in current ArenaBin
  RunCase* run_case_;  // corresponding RunCase for run (de-)allocation
  SizeClass* sc_;      // mutual conversion between slab class index and size
  // [the start address of a run, RunBits]
  std::unordered_map<void*, RunBits> runs_; // non-full(non-empty, half-used) runs for allocation
  typedef std::unordered_map<void*, RunBits>::iterator iterator;

  static constexpr size_t kBucketsCount = 32;
  static constexpr size_t kNSlabsPerGrp = SlabConst::kNSlabsPerGrp;
  static constexpr size_t kNSlabsCached = SlabConst::kNSlabsCached;
  static constexpr size_t kMaxSmallSize = SlabConst::kMaxSmallSize;
  static constexpr size_t kRunHoldCount = SlabConst::kRunHoldCount;
  static constexpr size_t kMaxSmallIndex = SizeClass::size2index_compute(kMaxSmallSize);

 public:
  ArenaBin() : arena_(-1), index_(-1), type_(kInvalid), run_case_(nullptr), sc_(nullptr) {}

  ~ArenaBin() {
    assert(type_ == kSmall || type_ == kMedium);
    for(auto& [run, bits] : runs_) {
      // release all runs whose regions are all free (not in use)
      // else inform the corresponding RunCase it has a half-used run
      bool release = bits.full();
      if(type_ == kSmall) {
        run_case_->srun_release(run, index_, release);
        if(!release) { bits.mark_polluted(type_); }
      } else {
        run_case_->mrun_release(run, index_, release);
        if(!release) { bits.mark_polluted(type_); }
      }
    }
  }

  ArenaBin(const ArenaBin&) = delete;

  ArenaBin& operator=(const ArenaBin&) = delete;

  /**
   * @brief ArenaBin initialization
   * @param arena the index of corresponding arena in the global arena array
   * @param index the ArenaBin index within bins array (slab size class index)
   * @param sc pointer to SizeClass for conversion between slab size and index
   * */
  void init(size_t arena, size_t index, RunCase* run_case, SizeClass* sc) {
    arena_ = arena, index_ = index, run_case_ = run_case, sc_ = sc;
    type_ = (index <= kMaxSmallIndex) ? kSmall : kMedium;
    assert(index < kNSlabsCached && run_case != nullptr);
    assert(sc != nullptr);
    runs_.reserve(kBucketsCount);
  }

  /**
   * @brief find a non-empty run in runs, or acquire a new run
   * @return an iterator to a run in runs
   * */
  iterator select_run() {
    assert(type_ == kSmall || type_ == kMedium);
    if(runs_.empty()) {
      if(type_ == kSmall) {
        auto [meta, run] = run_case_->srun_acquire(arena_, index_);
        assert((void*) meta == (void*) run);
        size_t rsize = SlabConst::kRunSizeTab[SlabConst::kCBin2RBin[index_ / kNSlabsPerGrp]];
        void* objs = (void*) ((uintptr_t) run + rsize - meta->count() * meta->size());
        if(run) runs_.insert({run, RunBits(meta->size(), meta->count(), meta, objs)});
      } else {
        auto [meta, run] = run_case_->mrun_acquire(arena_, index_);
        if(run) runs_.insert({run, RunBits(meta->size(), meta->count(), meta, run)});
      }
    }
    return runs_.begin();
  }

  /**
   * @brief restock regions for a certain CacheBin
   * @param regions the region container of the CacheBin
   * @param restock restock quantity
   * */
  void fill_cache_bin(std::deque<region_t>& regions, size_t restock) {
    LockGuard guard(lock_);
    while(regions.size() < restock) {
      auto rit = select_run();
      if(rit == runs_.end()) break; // no more runs
      RunBits& rbits = rit->second;
      rbits.fill_regions(regions, restock, type_);
      // no more regions in current run, remove it from hash map
      if(rbits.empty()) { runs_.erase(rit); }
    }
  }

  /**
   * @brief release a small region specified by the index back to current ArenaBin
   * @param run the start address of the corresponding run
   * @param index the region's index in the run
   * */
  void release(SmallMeta* meta, void* run, size_t index) {
    assert(type_ == kSmall && meta->size() == sc_->index2size(index_));
    LockGuard guard(lock_);
    auto it = runs_.find(run);
    if(it == runs_.end()) { // this extent has been fully allocated, reconstruct
      size_t rsize = SlabConst::kRunSizeTab[SlabConst::kCBin2RBin[index_ / kNSlabsPerGrp]];
      void* objs = (void*) ((uintptr_t) run + rsize - meta->count() * meta->size());
      it = runs_.insert({run, RunBits(meta->size(), meta->count(), meta, objs, true)}).first;
    }
    RunBits& rbits = it->second;
    rbits.dealloc_region(index);
    // release free run back to RunCase
    if(rbits.full() && runs_.size() > kRunHoldCount) {
      assert(run == it->first);
      runs_.erase(it), run_case_->srun_release(run, index_);
    }
  }

  /**
   * @brief release a medium region specified by the index back to current ArenaBin
   * @param run the start address of the corresponding run
   * @param index the region's index in the run
   * */
  void release(MediumMeta* meta, void* run, size_t index) {
    assert(type_ == kMedium);
    LockGuard guard(lock_);
    auto it = runs_.find(run);
    if(it == runs_.end()) { // this extent has been fully allocated, reconstruct
      it = runs_.insert({run, RunBits(meta->size(), meta->count(), meta, run, true)}).first;
    }
    RunBits& rbits = it->second;
    rbits.dealloc_region(index);
    // release free run back to RunCase
    if(rbits.full() && runs_.size() > kRunHoldCount) {
      assert(run == it->first);
      runs_.erase(it), run_case_->mrun_release(run, index_);
    }
  }
};


/**
 * @brief small & medium allocation management in a run
 * */
class Arena {
  static constexpr size_t kMaxSmallSize = SlabConst::kMaxSmallSize;
  static constexpr size_t kMaxMediumSize = SlabConst::kMaxMediumSize;
  static constexpr size_t kMaxLargeSize = SlabConst::kMaxLargeSize;
  static constexpr size_t kNSlabsCached = SlabConst::kNSlabsCached;

  std::atomic<int> nbinds_; // the number of threads bound to this arena, no need for lock ownership
  uint32_t index_;          // the index of current arena in the global arena array
  RunCase* run_case_;       // corresponding RunCase for run (de-)allocation
  SizeClass* sc_;           // mutual conversion between slab class index and size
  ArenaBin bins_[kNSlabsCached]; // the actual allocation management structure for a specific size class

 public:
  /**
   * @brief Arena initialization
   * @param index the index of current arena in the global arena array
   * @param run_case corresponding RunCase for run (de-)allocation
   * @param sc pointer to SizeClass for conversion between slab size and index
   * */
  Arena(size_t index, RunCase* run_case, SizeClass* sc) :
    nbinds_(0), index_(index), run_case_(run_case), sc_(sc), bins_{} {
    assert(run_case != nullptr && sc != nullptr);
    for(size_t bid = 0; bid < kNSlabsCached; bid++) {
      bins_[bid].init(index_, bid, run_case_, sc_);
    }
  }

  ~Arena() {
    assert(nbinds_ == 0);
  }

  Arena(const Arena&) = delete;

  Arena& operator=(const Arena&) = delete;

  /**
   * @brief when constructing a new thread cache and binding it to an arena, increment nbinds;
   * when destructing a thread cache and unbinding it from corresponding arena, decrement nbinds
   * */
  std::atomic<int>& nbinds() { return nbinds_; }

  /**
   * @brief restock regions for a certain CacheBin
   * @param regions the region container of the CacheBin
   * @param restock restock quantity
   * @param index the index of slab size class
   * */
  void fill_cache_bin(std::deque<region_t>& regions, size_t restock, size_t index) {
    assert(index < kNSlabsCached);
    bins_[index].fill_cache_bin(regions, restock);
  }

  /**
   * @brief release a small region back to current arena
   * @param meta the corresponding small run management meta
   * @param run the start address of the corresponding run
   * @param index the region's index in the run
   * */
  void release(SmallMeta* meta, void* run, size_t index) {
    assert(meta->arena() == index_);
    size_t bin_idx = sc_->size2index(meta->size());
    bins_[bin_idx].release(meta, run, index);
  }

  /**
   * @brief release a medium region back to current arena
   * @param meta the corresponding medium run management meta
   * @param run the start address of the corresponding run
   * @param index the region's index in the run
   * */
  void release(MediumMeta* meta, void* run, size_t index) {
    assert(meta->arena() == index_);
    size_t bin_idx = sc_->size2index(meta->size());
    bins_[bin_idx].release(meta, run, index);
  }

  /**
   * @brief allocate a large region
   * @param size the size of region
   * */
  region_t large_acquire(size_t size) {
    assert(size > kMaxMediumSize);
    assert(size <= kMaxLargeSize);
    return run_case_->large_acquire(size);
  }
};

}

#endif //SLABSTORE_ARENA_H
