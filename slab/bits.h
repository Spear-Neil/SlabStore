#ifndef SLABSTORE_BITS_H
#define SLABSTORE_BITS_H

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <deque>

#include "const.h"
#include "token.h"
#include "region.h"
#include "desc.h"
#include "util.h"

namespace SlabStore {

using util::roundup;
using util::index_least0;
using util::popcount;

/**
 * @brief in-memory structure for recording allocation status of regions in
 * a run, the use states (free/busy, use purpose ....) are out of care in this
 * structure, we only record which regions are allocated from a run (may be
 * used as persistent object, as volatile object, or even may reside in thread
 * local cache ...)
 * */
class RunBits {
  uint32_t size_;   // region size in bytes
  uint16_t count_;  // total regions count in the current run
  uint16_t alloc_;  // the number of regions allocated from the current run
  void* meta_;      // run meta of the run
  void* objs_;      // the start address of regions array in a run
  uint64_t bits_;   // bitmap for region allocation status
  // 1 means the corresponding region has been allocated from the run

  static constexpr size_t kUnitBits = 64;

 public:
  /**
   * @brief RunBits constructor
   * @param size region size in bytes
   * @param count total regions count in current run
   * @param meta the management meta of the run
   * @param objs the start address of regions array in the run
   * @param empty all objects are fully allocated in current run
   * */
  RunBits(size_t size, size_t count, void* meta, void* objs, bool empty = false) :
    size_(size), count_(count), alloc_(empty ? count : 0), meta_(meta), objs_(objs), bits_(0) {
    if(count > kUnitBits) { // more than 64 regions
      size_t nbytes = roundup(count, kUnitBits) / 8;
      bits_ = (uint64_t) malloc(nbytes);
      memset((void*) bits_, empty ? 0xFF : 0, nbytes);
      size_t remain = count % kUnitBits;
      if(empty && remain != 0) { ((uint64_t*) bits_)[count / kUnitBits] = (0x01ul << remain) - 1; }
    } else if(empty) { bits_ = (count == kUnitBits) ? ~0x00ul : (0x01ul << count) - 1; }
  }

  ~RunBits() { if(count_ > kUnitBits) free((void*) bits_); }

  RunBits(RunBits&& rbits) noexcept: size_(rbits.size_), count_(rbits.count_), alloc_(rbits.alloc_),
                                     meta_(rbits.meta_), objs_(rbits.objs_), bits_(rbits.bits_) {
    if(rbits.count_ > kUnitBits) rbits.bits_ = (uint64_t) nullptr;
  }

  RunBits& operator=(RunBits&& rbits) noexcept {
    size_ = rbits.size_, count_ = rbits.count_, alloc_ = rbits.alloc_;
    meta_ = rbits.meta_, objs_ = rbits.objs_, bits_ = rbits.bits_;
    if(rbits.count_ > kUnitBits) rbits.bits_ = (uint64_t) nullptr;
    return *this;
  }

  /**
   * @brief reload bits_ through meta
   * */
  void reload(SmallMeta* meta) {
    assert((void*) meta == meta_);
    assert(meta->count() == count_ && meta->size() == size_);
    if(count_ > kUnitBits) {
      assert(alloc_ == 0);
      for(size_t ind = 0; ind < count_; ind++) {
        size_t uid = ind / kUnitBits, idx = ind % kUnitBits;
        uint64_t& bits = ((uint64_t*) bits_)[uid];
        assert(idx != 0 || bits == 0);
        if(meta->token(ind)->busy()) {
          bits |= (0x01ul << idx), alloc_++;
        }
      }
    } else {
      assert(bits_ == 0 && alloc_ == 0);
      // may be optimized through SIMD instructions
      for(size_t ind = 0; ind < count_; ind++) {
        if(meta->token(ind)->busy()) {
          bits_ |= (0x01ul << ind), alloc_++;
        }
      }
    }
    assert(alloc_ >= 0 && alloc_ < count_);
  }

  /**
   * @brief reload bits_ through meta
   * */
  void reload(MediumMeta* meta) {
    assert((void*) meta == meta_ && count_ <= kUnitBits);
    assert(meta->count() == count_ && meta->size() == size_);
    assert(bits_ == 0 && alloc_ == 0);
    for(size_t ind = 0; ind < count_; ind++) {
      if(meta->token(ind)->busy()) {
        bits_ |= (0x01ul << ind), alloc_++;
      }
    }
    assert(alloc_ >= 0 && alloc_ < count_);
  }

  /**
   * @brief reload bits_ through desc
   * */
  void reload(ExtentDesc* desc) {
    assert((void*) desc == meta_ && count_ <= kUnitBits);
    assert(desc->count() == count_ && desc->size() == size_);
    assert(bits_ == 0 && alloc_ == 0);
    for(size_t ind = 0; ind < count_; ind++) {
      if(desc->token(ind)->busy()) {
        bits_ |= (0x01ul << ind), alloc_++;
      }
    }
    assert(alloc_ >= 0 && alloc_ < count_);
  }

  /**
   * @brief get the token of a region in the run
   * @param idx the index of the region in the run
   * @param type the region type
   * */
  token_t* locate_token(size_t idx, RegionType type) {
    if(type == kSmall) {
      assert(((SmallMeta*) meta_)->size() == size_);
      return ((SmallMeta*) meta_)->token(idx);
    } else if(type == kMedium) {
      assert(((MediumMeta*) meta_)->size() == size_);
      return ((MediumMeta*)
        meta_)->token(idx);
    } else {
      assert(type == kLarge && ((ExtentDesc*) meta_)->size() == size_);
      return ((ExtentDesc*)
        meta_)->token(idx);
    }
  }

  /**
   * @brief allocate regions from current run to fill a region list
   * @param regions the region list to be filled
   * @param restock the restock quantity of regions
   * @param type the region type
   * */
  void fill_regions(std::deque<region_t>& regions, size_t restock, RegionType type) {
    assert(alloc_ < count_ && (type == kSmall || type == kMedium));
    if(count_ <= kUnitBits) {
      assert(popcount(bits_) < count_ && popcount(bits_) == alloc_); // should not be empty
      while(regions.size() < restock) {
        int idx = index_least0(bits_);
        if(idx == count_ || idx == -1) break;   // no more regions

        token_t* token = locate_token(idx, type);
        void* region = (void*) ((uintptr_t) objs_ + idx * size_);
        regions.emplace_back(token, region);
        bits_ |= (0x01ul << idx), alloc_++;
      }
    } else { // more than 64 regions
      size_t unit = (count_ + kUnitBits - 1) / kUnitBits;
      size_t uid = 0, count = kUnitBits;

      while(regions.size() < restock) {
        uint64_t& bits = ((uint64_t*) bits_)[uid];

        while(regions.size() < restock) {
          int idx = index_least0(bits);
          if(idx == count || idx == -1) break;   // no more regions in the current unit
          token_t* token = locate_token(uid * kUnitBits + idx, type);
          void* region = (void*) ((uintptr_t) objs_ + (uid * kUnitBits + idx) * size_);
          regions.emplace_back(token, region);
          bits |= (0x01ul << idx), alloc_++;
        }

        if(++uid >= unit) break;  // the last unit
        if(uid == unit - 1) { count = count_ - uid * kUnitBits; }
      }
    }
  }

  /**
   * @brief deallocate small/medium regions back to current run
   * @param index the index of the region
   * */
  void dealloc_region(size_t index) {
    assert(alloc_ <= count_ && index < count_);
    if(count_ <= kUnitBits) {
      assert(popcount(bits_) == alloc_);
      assert(bits_ & (0x01ul << index));
      bits_ &= ~(0x01ul << index), alloc_--;
    } else {  // more than 64 regions
      size_t uid = index / kUnitBits, ind = index % kUnitBits;
      uint64_t& bits = ((uint64_t*) bits_)[uid];
      assert(bits & (0x01ul << ind));
      bits &= ~(0x01ul << ind), alloc_--;
    }
  }

  /**
   * @brief allocate a large region
   * */
  region_t alloc_large() {
    assert(alloc_ < count_ && count_ < kUnitBits);
    assert(popcount(bits_) < count_ && popcount(bits_) == alloc_); // should not be fully allocated
    int idx = index_least0(bits_);
    token_t* token = locate_token(idx, kLarge);
    void* region = (void*) ((uintptr_t) objs_ + idx * size_);
    bits_ |= (0x01ul << idx), alloc_++;
    return {token, region};
  }

  /**
   * @brief deallocate a large region, record its status
   * @param index the index of large region
   * */
  void dealloc_large(size_t index) {
    assert(alloc_ <= count_ && count_ < kUnitBits);
    assert(index < count_ && (bits_ & (0x01ul << index)));
    assert(popcount(bits_) == alloc_);
    bits_ &= ~(0x01ul << index), alloc_--;
  }

  /**
   * @brief persistently mark the run as polluted/half-used for fast reload
   * */
  void mark_polluted(RegionType type) {
    assert(type == kSmall || type == kMedium);
    if(type == kSmall) {
      RunCond& cond = ((SmallMeta*) meta_)->cond();
      cond = kPolluted;
      persist_write_back(&cond, sizeof(RunCond));
    } else {
      RunCond& cond = ((MediumMeta*)
        meta_)->cond();
      cond = kPolluted;
      persist_write_back(&cond, sizeof(RunCond));
    }
    // no waiting for finish, failure atomicity guaranteed by stat
  }

  /**
   * @brief whether the run is empty (all regions have been allocated)
   * */
  bool empty() const { return alloc_ == count_; }

  /**
   * @brief whether all regions have been released back to current run
   * */
  bool full() const { return alloc_ == 0; }
};


}

#endif //SLABSTORE_BITS_H
