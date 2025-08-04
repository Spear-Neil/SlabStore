/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_DESC_H
#define SLABSTORE_DESC_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <tuple>

#include "const.h"
#include "pptr.h"
#include "token.h"
#include "persist.h"
#include "util.h"

namespace SlabStore {

/* Term Definition & Persistent Memory Pool Structure
 * Files: an extent-file composed of Extent and a meta-file consisting of ExtentMeta and MediumMeta;
 * Extent: a fixed-size block of persistent memory, 4MB per extent
 * Run: a fixed-size block of persistent memory in an Extent, each Run is a slab class containing
 *      a certain amount of Regions w.r.t. a fixed-size class, each small and medium allocation is
 *      fulfilled by allocating a Region;
 * Region: a memory object for one allocation request
 *
 * Note: we borrow some of these concepts from jemalloc (an extensively used high-performance
 * slab memory allocator, url: https://github.com/jemalloc/jemalloc.git)
 *
 * Category of Allocation (quantum size: 32 Byte)
 * _____________________________________________________________________________________________________________________
 * |Category            | Spacing | Size                           | Gap  | Run  | Notes                               |
 * |____________________|_________|________________________________|______|______|_____________________________________|
 * |Small: [32, 2K]     | 32      | [32,    64,    96,    128]     | 128  | 16K  | Eager (Physical Page Allocation)    |
 * |                    | 32      | [160,   192,   224,   256]     | 256  | 16K  | physical alloc unit: extent         |
 * |                    | 64      | [320,   384,   448,   512]     | 512  | 32K  |                                     |
 * |                    | 128     | [640,   768,   896,   1K]      | 1K   | 32K  |                                     |
 * |                    | 256     | [1280,  1536,  1792,  2K]      | 2K   | 64K  |                                     |
 * |____________________|_________|________________________________|______|______|_____________________________________|
 * |Medium: (2K, 64K]   | 512     | [2560,  3K,    3584,  4K]      | 4K   | 128K | Eager (Physical Page Allocation)    |
 * |                    | 1K      | [5K,    6K,    7K,    8K]      | 8K   | 128K | physical alloc unit: run            |
 * |                    | 2K      | [10K,   12K,   14K,   16K]     | 16K  | 256K |                                     |
 * |                    | 4K      | [20K,   24K,   28K,   32K]     | 32K  | 256K |                                     |
 * |                    | 8K      | [40K,   48K,   56K,   64K]     | ...  | 512K |                                     |
 * |____________________|_________|________________________________|______|______|_____________________________________|
 * |Large: (64K, 4M]    | ...     | [128K, 256K, 512K, 1M, 2M, 4M] | ...  | 4M   | Lazy (Physical Page Allocation)     |
 * |____________________|_________|________________________________|______|______|_____________________________________|
 */

using util::index_least0;
using util::popcount;

enum RegionType : uint32_t { kSmall, kMedium, kLarge, kInvalid, kTypeCount };

/**
 * @brief Run's usage condition after normal allocator close
 * @details only valid when the allocator is normally closed and the run is allocated, kPure is the
 * default value means the regions in the run are all used, kPolluted means the run is half-used
 */
enum RunCond : uint8_t { kPure, kPolluted };

class alignas(kCacheLineSize) ExtentDesc {
  RegionType type_;       // region type, means how to use an extent
  uint32_t rcase_;        // index of the corresponding RunCase in the global RunCase array
  uint32_t size_;         // run size (small & medium) or region size (large)
  uint32_t count_;        // total run number or region number
  pptr64_t next_;         // points to next free extent descriptor
  pptr64_t runs_;         // for Medium allocation, points to MediumMeta array
  union {
    uint64_t small_[4];   // for small allocation, run size: 16K/32k/64k, every single bit corresponds to one run
    uint8_t medium_[32];  // for medium allocation, run size: 128k/256k/512k, every single byte corresponds to one run
    token_t large_[32];   // for large allocation as persistent token, (64K, 4M], every single byte corresponds to one region
  };

  static constexpr size_t kExtentSize = SlabConst::kExtentSize;

 public:
  ExtentDesc() = delete;

  ~ExtentDesc() = delete;

  ExtentDesc(const ExtentDesc&) = delete;

  ExtentDesc& operator=(const ExtentDesc&) = delete;

  /**
   * @brief persistently initialize an extent descriptor
   * @param type region type, means how to use an extent
   * @param run_case the index of RunCase
   * @param size run size (small & medium) or region size (large)
   * @param next the next free extent descriptor
   * @param runs the pointer to the region management meta array (medium allocation)
   * @param wait wait until the cache line reaching the persistence domain
   * */
  void construct(RegionType type, size_t run_case, size_t size, void* next, void* runs, bool wait) {
    assert(type == kSmall || type == kMedium || type == kLarge);
    assert(size <= kExtentSize && kExtentSize % size == 0);
    assert(type != kMedium || runs != nullptr);
    type_ = type, rcase_ = run_case, size_ = size;
    count_ = kExtentSize / size, next_ = next, runs_ = runs;
    switch(type) {
      case kSmall:
        memset(small_, 0, 32);
        break;
      case kMedium:
        memset(medium_, 0, 32);
        break;
      case kLarge:
        for(auto& token : large_) token.fire(false);
        break;
      default:
        fprintf(stderr, "[ERROR]: invalid DescType %i\n", type);
        exit(EXIT_FAILURE);
    }
    persist_write_back(this, sizeof(ExtentDesc));
    if(wait) { persist_wait_finish(); }
    assert(size_t(this) % kCacheLineSize == 0);
  }

  /**
   * @brief check if all regions are used
   * */
  bool half_used() {
    assert(type_ == kLarge);
    for(size_t ind = 0; ind < count_; ind++) {
      if(!large_[ind].busy()) return true;
    }
    return false;
  }

  /**
   * @brief region type in current extent
   * */
  RegionType& type() { return type_; }

  /**
   * @brief the index of RunCase which the extent belongs to
   * */
  size_t rcase() const { return rcase_; }

  /**
   * @brief run size (small & medium) or region size (large)
   * */
  size_t size() const { return size_; }

  /**
   * @brief total run number or region number
   * */
  size_t count() const { return count_; }

  /**
   * @brief pointer to the next free ExtentDesc
   * */
  pptr64_t& next() { return next_; }

  /**
   * @brief the start address of MediumMeta array
   * */
  void* runs() { return runs_; }

  /**
   * @brief get token by index
   * */
  token_t* token(size_t idx) {
    assert(idx < count_ && type_ == kLarge);
    return &large_[idx];
  }

  /**
   * @brief whether there any small runs are in use
   * */
  bool srun_busy() {
    assert(type_ == kSmall && count_ % 64 == 0);
    size_t unit = count_ / 64;
    for(size_t uid = 0; uid < unit; uid++) {
      if(popcount(small_[uid]) != 0) return true;
    }
    return false;
  }

  /**
   * @brief whether there any medium runs are in use
   * */
  bool mrun_busy() {
    assert(type_ == kMedium && count_ <= 32);
    for(size_t rid = 0; rid < count_; rid++) {
      if(medium_[rid] != 0) return true;
    }
    return false;
  }

  /**
   * @brief check if a small run is allocated
   * @param ind the index to the small run
   * */
  bool small_used(size_t ind) {
    assert(ind < count_ && type_ == kSmall);
    size_t uid = ind / 64, idx = ind % 64;
    uint64_t mask = 0x01ul << idx;
    return mask & small_[uid];
  }

  /**
   * @brief check if a medium run is allocated
   * @param ind the index to the medium run
   * */
  bool medium_used(size_t ind) {
    assert(ind < count_ && type_ == kMedium);
    return medium_[ind];
  }

  /**
   * @brief locate a free small run
   * @param ind the start index to check
   * @return the index of the free small run
   * */
  size_t locate_fsrun(size_t ind = 0) {
    assert(ind < count_);
    assert(type_ == kSmall && count_ % 64 == 0);
    size_t unit = count_ / 64;
    for(size_t uid = ind / 64; uid < unit; uid++) {
      size_t idx = index_least0(small_[uid]);
      if(idx == -1) continue; // no more free run in current unit
      return uid * 64 + idx;
    }
    return count_;  // no more free run
  }

  /**
   * @brief persistently allocate a free small run
   * @param rid the index of the free small run
   * */
  void palloc_srun(size_t rid) {
    assert(type_ == kSmall && rid < count_);
    size_t uid = rid / 64, idx = rid % 64;
    assert((small_[uid] & (0x01ul << idx)) == 0);
    small_[uid] = small_[uid] | (0x01ul << idx);
    assert(uintptr_t(this) % kCacheLineSize == 0);
    wait_write_back(this, sizeof(ExtentDesc));
  }

  /**
   * @brief persistently deallocate a small run
   * @param rid the index of the run
   * */
  void pdealloc_srun(size_t rid) {
    assert(type_ == kSmall && rid < count_);
    size_t uid = rid / 64, idx = rid % 64;
    assert((small_[uid] & (0x01ul << idx)) != 0);
    small_[uid] = small_[uid] & ~(0x01ul << idx);
    wait_write_back(this, sizeof(ExtentDesc));
  }

  /**
   * @brief locate a free medium run
   * @param ind the start index to check
   * @return the index of the free medium run
   * */
  size_t locate_fmrun(size_t ind = 0) {
    assert(ind < count_);
    assert(type_ == kMedium && count_ <= 32);
    for(size_t rid = ind; rid < count_; rid++) {
      if(medium_[rid] == 0) return rid;
    }
    return count_; // no more free run
  }

  /**
   * @brief persistently allocate a free medium run
   * @param rid the index of the free medium run
   * */
  void palloc_mrun(size_t rid) {
    assert(type_ == kMedium && rid < count_);
    assert(medium_[rid] == 0);
    medium_[rid] = 1;
    assert(uintptr_t(this) % kCacheLineSize == 0);
    wait_write_back(this, sizeof(ExtentDesc));
  }

  /**
   * @brief persistently deallocate a medium run
   * @param rid the index of the run
   * */
  void pdealloc_mrun(size_t rid) {
    assert(type_ == kMedium && rid < count_);
    assert(medium_[rid] == 1);
    medium_[rid] = 0;
    wait_write_back(this, sizeof(ExtentDesc));
  }
};

static_assert(sizeof(ExtentDesc) == kCacheLineSize);


// region management meta of a run for small allocation, located at the head of a run
// the last small region is aligned to the end of the run
class SmallMeta {
  uint32_t size_;     // region size
  uint16_t arena_;    // the index of corresponding arena in global arena array
  uint16_t count_;    // total region number
  pptr64_t next_;     // the next management meta
  RunCond cond_;      // usage condition of the run, for fast reboot after normal close
  token_t tokens_[];  // for small allocation as persistent token

  // todo: using free space for adjusting the layout of tokens to avoid cache line flush at the same line
 public:
  SmallMeta() = delete;

  /**
   * @brief initialize a small region management meta before persistent run allocation
   * @param arena the index of arena which the run belongs to
   * @param size region size
   * @param count total region number
   * */
  void construct(size_t arena, size_t size, size_t count) {
    size_ = size, arena_ = arena, count_ = count, next_ = nullptr, cond_ = kPure;
    for(size_t idx = 0; idx < count; idx++) tokens_[idx].fire(false);
    wait_write_back(this, sizeof(SmallMeta) + count * sizeof(token_t));
  }

  /**
   * @brief check if all regions are used
   * */
  bool half_used() {
    for(size_t ind = 0; ind < count_; ind++) {
      if(!tokens_[ind].busy()) return true;
    }
    return false;
  }

  /**
   * @brief region size
   * */
  size_t size() const { return size_; }

  /**
   * @brief the index of arena which the run belongs to
   * */
  size_t arena() const { return arena_; }

  /**
   * @brief total region number
   * */
  size_t count() const { return count_; }

  /**
   * @brief the run usage condition
   * */
  RunCond& cond() { return cond_; }

  /**
   * @brief get token by index
   * */
  token_t* token(size_t idx) {
    assert(idx < count_);
    return &tokens_[idx];
  }
};


// region management meta of a run for medium allocation, located at the tail of meta-file
class alignas(kCacheLineSize) MediumMeta {
  uint32_t size_;      // region size
  uint16_t arena_;     // the index of corresponding arena in global arena array
  uint16_t count_;     // total region number
  pptr32_t next_;      // the next region management meta
  RunCond cond_;       // usage condition of the run, for fast reboot after normal close
  token_t tokens_[51]; // for medium allocation as persistent token

 public:
  MediumMeta() = delete;

  /**
   * @brief initialize a medium region management meta before persistent run allocation
   * @param arena the index of arena which the run belongs to
   * @param size region size
   * @param count total region number
   */
  void construct(size_t arena, size_t size, size_t count) {
    assert(count <= 51);
    size_ = size, arena_ = arena, count_ = count, next_ = nullptr, cond_ = kPure;
    for(int ind = 0; ind < count; ind++) tokens_[ind].fire(false);
    wait_write_back(this, sizeof(MediumMeta));
  }

  /**
   * @brief check if all regions are used
   * */
  bool half_used() {
    for(size_t ind = 0; ind < count_; ind++) {
      if(!tokens_[ind].busy()) return true;
    }
    return false;
  }

  /**
   * @brief region size
   * */
  size_t size() const { return size_; }

  /**
   * @brief the index of arena which the run belongs to
   * */
  size_t arena() const { return arena_; }

  /**
   * @brief total region number
   * */
  size_t count() const { return count_; }

  /**
   * @brief the next management meta
   * */
  pptr32_t& next() { return next_; }

  /**
   * @brief the run usage condition
   * */
  RunCond& cond() { return cond_; }

  /**
   * @brief get token by index
   * */
  token_t* token(size_t idx) {
    assert(idx < count_);
    return &tokens_[idx];
  }
};

static_assert(sizeof(MediumMeta) == kCacheLineSize);


// (descriptor, the start address of this extent)
typedef std::pair<ExtentDesc*, void*> extent_t;
// (meta, the start address of this run)
typedef std::pair<SmallMeta*, void*> srun_t;  // region array does not start at the head, SmallMeta starts at the head
typedef std::pair<MediumMeta*, void*> mrun_t; // region array starts at the head, MediumMeta locates in the meta file
typedef std::pair<ExtentDesc*, void*> lrun_t; // region array starts at the head, ExtentDesc locates in the meta file

}

#endif //SLABSTORE_DESC_H
