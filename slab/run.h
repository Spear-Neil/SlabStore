/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_RUN_H
#define SLABSTORE_RUN_H

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <tuple>
#include <map>

#include "const.h"
#include "extent.h"
#include "desc.h"
#include "bits.h"
#include "size.h"
#include "util.h"

namespace SlabStore {

using util::MutexLock;
using util::LockGuard;
using util::popcount;
using util::index_most1;
using util::rounddown;

/**
 * @brief a memory container for accommodating runs of a specific size,
 * the real structure for run (de-)allocation in extents
 * */
class RunBin {
  MutexLock<> lock_;      // run (de-)allocation needs to hold this lock
  uint32_t rcase_;        // index of the corresponding RunCase in the global RunCase array
  RegionType type_;       // region type of extents in current RunBin
  uint32_t rsize_;        // run size
  ExtentCase* ext_case_;  // extent (de-)allocation
  std::unordered_map<void*, ExtentDesc*> exts_; // (start address of an extent, its descriptor)
  typedef std::unordered_map<void*, ExtentDesc*>::iterator iterator;

  static constexpr size_t kBucketsCount = 32;
  static constexpr size_t kExtentSize = SlabConst::kExtentSize;
  static constexpr size_t kMaxSmallSize = SlabConst::kMaxSmallSize;
  static constexpr size_t kMaxMediumSize = SlabConst::kMaxMediumSize;
  static constexpr size_t kExtentHoldCount = SlabConst::kExtentHoldCount;

 private:
  /**
   * @brief select an extent from the RunBin or if the RunBin is empty, acquire a new extent
   * */
  iterator select_ext() {
    if(exts_.empty()) {
      auto [desc, ext] = ext_case_->acquire(type_, rcase_, rsize_);
      if(ext) exts_.insert({ext, desc}); // make sure get a valid extent
    }
    return exts_.begin();
  }

  /**
   * @brief compute the number of regions in a run for small allocation
   * @param size region size in current run
   * @return the number of regions in a run
   * */
  size_t small_region_count(size_t size) {
    assert(type_ == kSmall);
    size_t count = rsize_ / size;
    while(true) {
      size_t meta_size = sizeof(SmallMeta) + count * sizeof(token_t);
      size_t total_size = count * size + meta_size;
      if(total_size <= rsize_ && rsize_ - total_size <= size) break;
      count = (rsize_ - meta_size) / size;
    }
    return count;
  }

  /**
    * @brief compute the number of regions in a run for medium allocation
    * @param size region size in current run
    * @return the number of regions in a run
    * */
  size_t medium_region_count(size_t size) {
    assert(type_ == kMedium);
    size_t count = rsize_ / size;
    // make region array size aligned to kPageSize for better physical space usage,
    // drawbacks: some cost of virtual address space wasting
    while(count * size % kPageSize != 0) count--;
    return count;
  }

 public:
  RunBin() : lock_(), rcase_(-1), type_(kInvalid), rsize_(-1), ext_case_(nullptr), exts_() {}

  ~RunBin() {
    assert(type_ == kSmall || type_ == kMedium);
    for(auto [ext, desc] : exts_) {
      assert(type_ == desc->type());
      // if any run in the extent is in-use, record it before allocator close
      // else release the extent back to ExtentCase
      bool record = (type_ == kSmall) ? desc->srun_busy() : desc->mrun_busy();
      if(record) ext_case_->record(desc);
      else ext_case_->release(desc, ext);
    }
  }

  RunBin(const RunBin&) = delete;

  RunBin& operator=(const RunBin&) = delete;

  /**
   * @brief RunBin initialization
   * @param rcase the index of corresponding RunCase
   * @param type the region type of extents in current RunBin
   * @param size the size of run in current RunBin
   * @param ext_case pointer to the global ExtentCase for extent (de-)allocation
   * */
  void init(size_t rcase, RegionType type, size_t size, ExtentCase* ext_case) {
    assert(ext_case != nullptr && (type == kSmall || type == kMedium));
    rcase_ = rcase, type_ = type, rsize_ = size, ext_case_ = ext_case;
    exts_.reserve(kBucketsCount);
  }

  /**
   * @brief reload a half-used extent (including free regions)
   * @param desc descriptor to the extent (kSmall)
   * */
  void small_reboot(ExtentDesc* desc) {
    assert(type_ == kSmall && desc->size() == rsize_);
    LockGuard guard(lock_);
    // check if the extent has any runs free
    if(desc->locate_fsrun() != desc->count()) {
      exts_.insert({ext_case_->extent(desc), desc});
    }
  }

  /**
   * @brief reload a half-used extent (including free regions)
   * @param desc descriptor to the extent (kMedium)
   * */
  void medium_reboot(ExtentDesc* desc) {
    assert(type_ == kMedium && desc->size() == rsize_);
    LockGuard guard(lock_);
    // check if the extent has any runs free
    if(desc->locate_fmrun() != desc->count()) {
      exts_.insert({ext_case_->extent(desc), desc});
    }
  }

  /**
   * @brief allocate a run for small allocation
   * @param arena the index of arena in the global arena array
   * @param size region size in current run
   * @return (meta of run, the start address of run)
   * */
  srun_t srun_acquire(size_t arena, size_t size) {
    assert(type_ == kSmall && size <= kMaxSmallSize);
    LockGuard guard(lock_);
    auto it = select_ext();
    if(it == exts_.end()) return {nullptr, nullptr}; // no more space

    auto [ext, desc] = *it;
    assert(desc->type() == kSmall);
    size_t rid = desc->locate_fsrun();
    assert(rid < desc->count()); // ext should not be fully allocated

    void* run = (void*) ((uintptr_t) ext + rid * rsize_);
    size_t count = small_region_count(size);
    ((SmallMeta*) run)->construct(arena, size, count);
    desc->palloc_srun(rid); // failure atomic
    // check whether there are any free small runs
    if(desc->locate_fsrun(rid) == desc->count()) exts_.erase(it);

    return {(SmallMeta*) run, run};
  }

  /**
   * @brief allocate a run for medium allocation
   * @param arena the index of arena in the global arena array
   * @param size region size in current run
   * @return (meta of run, the start address of run)
   * */
  mrun_t mrun_acquire(size_t arena, size_t size) {
    // todo: physical page pre-allocation for runs of different size
    assert(type_ == kMedium);
    assert(size > kMaxSmallSize && size <= kMaxMediumSize);
    LockGuard guard(lock_);
    auto it = select_ext();
    if(it == exts_.end()) return {nullptr, nullptr}; // no more space

    auto [ext, desc] = *it;
    assert(desc->type() == kMedium);
    size_t rid = desc->locate_fmrun();
    assert(rid < desc->count());// ext should not be fully allocated

    auto* meta = (MediumMeta*) desc->runs() + rid;
    void* run = (void*) ((uintptr_t) ext + rid * rsize_);
    size_t count = medium_region_count(size);
    meta->construct(arena, size, count);
    desc->palloc_mrun(rid);  // failure atomic
    // check whether there are any free medium runs
    if(desc->locate_fmrun(rid) == desc->count()) exts_.erase(it);

    return {meta, run};
  }

  /**
   * @brief release a run (small allocation) back to current RunBin
   * @param run the start address of the run
   * @param release whether to persistently release the run or just inform the corresponding RunBin that there is
   * a half-used run belongs to current RunCase-RunBin
   * */
  void srun_release(void* run, bool release) {
    assert(type_ == kSmall);
    LockGuard guard(lock_);
    ExtentDesc* desc = ext_case_->descriptor(run);
    assert(desc->size() == rsize_ && (uintptr_t) run % rsize_ == 0);
    assert(desc->type() == type_ && desc->rcase() == rcase_);
    void* ext = (void*) rounddown((uintptr_t) run, kExtentSize);
    auto it = exts_.find(ext);
    if(it == exts_.end()) exts_.insert({ext, desc});
    size_t rid = ((uintptr_t) run - (uintptr_t) ext) / rsize_;

    if(release) desc->pdealloc_srun(rid);
    if(!desc->srun_busy() && exts_.size() > kExtentHoldCount) {
      assert(it->first == ext && release == true);
      exts_.erase(it), ext_case_->release(desc, ext);
    }
  }

  /**
   * @brief release a run (medium allocation) back to current RunBin
   * @param run the start address of the run
   * @param release whether to persistently release the run or just inform the corresponding RunBin that there is
   * a half-used run belongs to current RunCase-RunBin
   * */
  void mrun_release(void* run, bool release) {
    assert(type_ == kMedium);
    LockGuard guard(lock_);
    ExtentDesc* desc = ext_case_->descriptor(run);
    assert(desc->size() == rsize_ && (uintptr_t) run % rsize_ == 0);
    assert(desc->type() == type_ && desc->rcase() == rcase_);
    void* ext = (void*) rounddown((uintptr_t) run, kExtentSize);
    auto it = exts_.find(ext);
    if(it == exts_.end()) exts_.insert({ext, desc});
    size_t rid = ((uintptr_t) run - (uintptr_t) ext) / rsize_;

    if(release) desc->pdealloc_mrun(rid);
    if(!desc->mrun_busy() && exts_.size() > kExtentHoldCount) {
      assert(it->first == ext && release == true);
      exts_.erase(it), ext_case_->release(desc, ext);
    }
  }
};


/**
 * @brief in-memory map structure for managing large regions
 * */
class LargeBin {
  MutexLock<> lock_;      // large region (de-)allocation needs to hold this lock
  uint32_t rcase_;        // index of the corresponding RunCase in the global RunCase array
  uint32_t size_;         // max supported large region size
  ExtentCase* ext_case_;  // extent (de-)allocation
  // [the start address of a run, RunBits]
  std::unordered_map<void*, RunBits> runs_; // non-full (non-empty, half-used) runs (extents) for large allocation
  typedef std::unordered_map<void*, RunBits>::iterator iterator;

  static constexpr size_t kBucketCount = 32;
  static constexpr size_t kMaxMediumSize = SlabConst::kMaxMediumSize;
  static constexpr size_t kMaxLargeSize = SlabConst::kMaxLargeSize;
  static constexpr size_t kExtentSize = SlabConst::kExtentSize;
  static constexpr size_t kExtentHoldCount = SlabConst::kExtentHoldCount;

 public:
  LargeBin() : lock_(), rcase_(-1), size_(-1), ext_case_(nullptr) {}

  ~LargeBin() {
    for(auto& [ext, bits] : runs_) {
      assert((uintptr_t) ext % kExtentSize == 0);
      ExtentDesc* desc = ext_case_->descriptor(ext);
      assert(desc->size() == size_ && desc->type() == kLarge);
      // release extents whose regions are all free, record half-used extents
      if(!bits.full()) ext_case_->record(desc);
      else ext_case_->release(desc, ext);
    }
  }

  LargeBin(const LargeBin&) = delete;

  LargeBin& operator=(const LargeBin&) = delete;

  /**
   * @brief LargeBin initialization
   * @param rcase the index of corresponding RunCase in the global RunCase array
   * @param size max supported large region size
   * @param ext_case pointer to the global ExtentCase for extent (de-)allocation
   * */
  void init(size_t rcase, size_t size, ExtentCase* ext_case) {
    assert(popcount(size) == 1 && size > kMaxMediumSize);
    assert(size <= kMaxLargeSize && ext_case != nullptr);
    rcase_ = rcase, size_ = size, ext_case_ = ext_case;
    runs_.reserve(kBucketCount);
  }

  /**
   * @brief reload a half-used run (kLarge extent) into current LargeBin
   * @param desc descriptor to the half-used run (kLarge extent)
   * */
  void reboot(ExtentDesc* desc) {
    assert(desc->size() == size_ && desc->type() == kLarge);
    void* ext = ext_case_->extent(desc);
    LockGuard guard(lock_);
    auto [it, ins] = runs_.insert({ext, RunBits(desc->size(), desc->count(), desc, ext)});
    assert(ins == true);
    it->second.reboot(desc);
  }

  /**
   * @brief allocate a large region
   * */
  region_t acquire() {
    LockGuard guard(lock_);
    if(runs_.empty()) {
      auto [desc, ext] = ext_case_->acquire(kLarge, rcase_, size_);
      if(ext == nullptr) return {nullptr, nullptr};
      assert(size_ == desc->size() && desc->rcase() == rcase_);
      runs_.insert({ext, RunBits(desc->size(), desc->count(), desc, ext)});
    }
    assert(!runs_.empty());
    auto it = runs_.begin();
    RunBits& rbits = it->second;
    region_t region = rbits.alloc_large();
    if(rbits.empty()) { runs_.erase(it); } // no more regions available in the current run
    return region;
  }

  /**
   * @brief release a large region
   * @param desc the corresponding extent descriptor
   * @param ptr the start address of the region
   * */
  void release(ExtentDesc* desc, void* ptr) {
    assert(desc->size() == size_ && desc->rcase() == rcase_);
    // todo: physical page release
    void* ext = (void*) rounddown((uintptr_t) ptr, kExtentSize);
    size_t ind = ((uintptr_t) ptr - (uintptr_t) ext) / desc->size(); // region index
    assert(ind < desc->count());
    desc->token(ind)->fire();  // persistently mark this region as free
    bool free_ext = false;
    {
      LockGuard guard(lock_);
      auto it = runs_.find(ext);
      if(it == runs_.end()) { // this extent has been fully allocated, reconstruct it
        it = runs_.insert({ext, RunBits(desc->size(), desc->count(), desc, ext, true)}).first;
      }
      RunBits& rbits = it->second;
      rbits.dealloc_large(ind);
      // release free extent back to ExtentCase
      if(rbits.full() && runs_.size() > kExtentHoldCount) {
        runs_.erase(it), free_ext = true;
      }
    }
    if(free_ext) { ext_case_->release(desc, ext); }
  }
};


/**
 * @brief A RunCase acquires extents from the ExtentCase and divides extents into small runs
 * for small and medium allocation requests. For large allocation requests, it acquires an
 * extent and divides it into regions.
 * */
class RunCase {
  static constexpr size_t kRunTypeCount = SlabConst::kRunTypeCount;
  static constexpr size_t kNSlabsCached = SlabConst::kNSlabsCached;
  static constexpr size_t kMaxSmallSize = SlabConst::kMaxSmallSize;
  static constexpr size_t kMaxMediumSize = SlabConst::kMaxMediumSize;
  static constexpr size_t kMaxLargeSize = SlabConst::kMaxLargeSize;
  static constexpr size_t kMaxSmallIndex = SizeClass::size2index_compute(kMaxSmallSize);
  static constexpr size_t kNSlabsPerGrp = SlabConst::kNSlabsPerGrp;
  static constexpr size_t kSRunBinBound = SlabConst::kCBin2RBin[kMaxSmallIndex / kNSlabsPerGrp];
  static constexpr size_t kLargeTypeCount = popcount(kMaxLargeSize - kMaxMediumSize);

  uint32_t index_;        // the index of current RunCase in the global RunCase array
  ExtentCase* ext_case_;  // extent (de-)allocation
  SizeClass* sc_;         // mutual conversion between slab size and index
  std::map<std::pair<RegionType, size_t>, size_t> desc2rbid_; // [region type, run size] -> run bin index, for reboot

  RunBin rbins_[kRunTypeCount];     // the actual run allocation structure for a specific run size
  LargeBin lbins_[kLargeTypeCount]; // the actual large region allocation structure

 public:
  RunCase(size_t index, ExtentCase* ext_case, SizeClass* sc) :
    index_(index), ext_case_(ext_case), sc_(sc), rbins_{}, lbins_{} {
    assert(ext_case != nullptr);
    for(size_t rbid = 0; rbid < kRunTypeCount; rbid++) {
      size_t run_size = SlabConst::kRunSizeTab[rbid];
      RegionType type = kSmall;
      if(rbid > kSRunBinBound) type = kMedium;
      desc2rbid_.insert({{type, run_size}, rbid});
      rbins_[rbid].init(index_, type, run_size, ext_case_);
    }
    for(size_t lbin = 0; lbin < kLargeTypeCount; lbin++) {
      size_t size = kMaxMediumSize << (lbin + 1);
      lbins_[lbin].init(index_, size, ext_case_);
    }
  }

  ~RunCase() = default;

  RunCase(const RunCase&) = delete;

  RunCase& operator=(const RunCase&) = delete;

  /**
   * @brief reload a half-used extent into current RunCase
   * @param desc descriptor to the half-used extent (kSmall)
   * */
  void small_reboot(ExtentDesc* desc) {
    assert(desc->rcase() == index_ && desc->type() == kSmall);
    size_t rbid = desc2rbid_[{desc->type(), desc->size()}];
    rbins_[rbid].small_reboot(desc);
  }

  /**
   * @brief reload a half-used extent into current RunCase
   * @param desc descriptor to the half-used extent (kSmall)
   * */
  void medium_reboot(ExtentDesc* desc) {
    assert(desc->rcase() == index_ && desc->type() == kMedium);
    size_t rbid = desc2rbid_[{desc->type(), desc->size()}];
    rbins_[rbid].medium_reboot(desc);
  }

  /**
   * @brief reload a half-used run (kLarge extent) into current RunCase
   * @param desc descriptor to the half-used run (kLarge extent)
   * */
  void large_reboot(ExtentDesc* desc) {
    assert(desc->type() == kLarge && desc->rcase() == index_);
    size_t bid = index_most1((desc->size() - 1) / kMaxMediumSize);
    assert(bid < kLargeTypeCount);
    lbins_[bid].reboot(desc);
  }

  /**
   * @brief allocate a run for small allocation
   * @param arena the index of arena in the global arena array
   * @param index the slab size class index
   * */
  srun_t srun_acquire(size_t arena, size_t index) {
    assert(index <= kMaxSmallIndex);
    size_t rbid = SlabConst::kCBin2RBin[index / kNSlabsPerGrp];
    size_t size = sc_->index2size(index);
    assert(rbid < kRunTypeCount && size <= kMaxSmallSize);
    return rbins_[rbid].srun_acquire(arena, size);
  }

  /**
   * @brief release a run (small allocation) back to current RunCase
   * @param run the start address of the run
   * @param index the slab size class index
   * @param release whether to persistently release the run or just inform the corresponding RunBin that there is
   * a half-used run belongs to current RunCase-RunBin
   * */
  void srun_release(void* run, size_t index, bool release = true) {
    assert(index <= kMaxSmallIndex);
    assert(ext_case_->descriptor(run)->rcase() == index_);
    size_t rbid = SlabConst::kCBin2RBin[index / kNSlabsPerGrp];
    assert(rbid < kRunTypeCount);
    rbins_[rbid].srun_release(run, release);
  }

  /**
   * @brief allocate a run for medium allocation
   * @param arena the index of arena in the global arena array
   * @param index the slab size class index
   * */
  mrun_t mrun_acquire(size_t arena, size_t index) {
    assert(index > kMaxSmallIndex && index < kNSlabsCached);
    size_t rbid = SlabConst::kCBin2RBin[index / kNSlabsPerGrp];
    assert(rbid < kRunTypeCount);
    size_t size = sc_->index2size(index);
    assert(size > kMaxSmallSize && size <= kMaxMediumSize);
    return rbins_[rbid].mrun_acquire(arena, size);
  }

  /**
   * @brief release a run (medium allocation) back to current RunCase
   * @param run the start address of the run
   * @param index the slab size class index
   * @param release whether to persistently release the run or just inform the corresponding RunBin that there is a
   * half-used run belongs to current RunCase-RunBin
   * */
  void mrun_release(void* run, size_t index, bool release = true) {
    assert(index > kMaxSmallIndex && index < kNSlabsCached);
    assert(ext_case_->descriptor(run)->rcase() == index_);
    size_t rbid = SlabConst::kCBin2RBin[index / kNSlabsPerGrp];
    assert(rbid < kRunTypeCount);
    rbins_[rbid].mrun_release(run, release);
  }

  /**
   * @brief allocate a large region
   * @param size the size of region
   * */
  region_t large_acquire(size_t size) { // todo: physical page pre-allocation
    assert(size > kMaxMediumSize && size <= kMaxLargeSize);
    size_t bid = index_most1((size - 1) / kMaxMediumSize);
    assert(bid < kLargeTypeCount);
    return lbins_[bid].acquire();
  }

  /**
   * @brief return a large region back to memory pool
   * */
  void large_release(ExtentDesc* desc, void* ptr) {
    assert(desc->type() == kLarge && desc->rcase() == index_);
    assert(desc->size() > kMaxMediumSize && desc->size() <= kMaxLargeSize);
    size_t bid = index_most1((desc->size() - 1) / kMaxMediumSize);
    assert(bid < kLargeTypeCount);
    lbins_[bid].release(desc, ptr);
  }
};

}

#endif //SLABSTORE_RUN_H
