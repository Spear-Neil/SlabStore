/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_TOKEN_H
#define SLABSTORE_TOKEN_H

#include <cstddef>
#include <cstdint>
#include <atomic>

#include "const.h"
#include "persist.h"

namespace SlabStore {

class token_t {
  std::atomic<uint8_t> token_;
  // Layout of token: |  inuse (1bit)  |  mode (1bit)          |  marker (6 bits)    |
  // assignment:      |  free/busy     |  volatile/persistent  |  user-defined tags  |
  // The first significant bit is used as occupation marker; zero for free, one for being used;
  // The second significant bit is used as use mode marker; zero for use the persistent memory
  // object as volatile memory object (when system crashes [StatCode: kVolatile], all contents
  // in the memory object is view as damaged or invalid, so when rebuilding allocator's meta-info,
  // such memory object should be released; after normal system shutdown, data in such objects can
  // be viewed as consistent); one for persistent memory object (user is in charge of maintaining
  // data consistency through a programming model similar to reserve-publish, after system crashes
  // due to power failure, we presume that data in such persistent memory object is consistent)
  // The remaining six bits are used as user-defined marker (e.g. classification tags for different
  // customized memory objects, 0 - 63)

  // as for allocator, it only cares about whether an object is allocated (inuse), so all objects
  // marked with inuse bit need to be processed appropriately when rebuilding user-defined data
  // structures after system crashes, regardless of whether an object is marked with persistent.

  static constexpr uint8_t kInuseBit = 0x80;
  static constexpr uint8_t kModeBit = 0x40;
  static constexpr uint8_t kMarkBits = 0x3F;

  // the region is free/unused
  static constexpr uint8_t kFreeCode = 0x00;

  // ensure every load/store operation is compiled as memory operation
  // data consistency and memory ordering are maintaining by upper level logic
  static constexpr std::memory_order load_order = std::memory_order_relaxed;
  static constexpr std::memory_order store_order = std::memory_order_relaxed;

  static constexpr bool kEnhancedADR = SlabConst::kEnhancedADR;

 public:
  token_t() = delete;

  ~token_t() = default;

  token_t(const token_t&) = delete;

  token_t& operator=(const token_t&) = delete;

  /**
   * @brief to hire corresponding region, mark this region as used with use mode and user-defined tag
   * @param mode use mode, true for persistent object, false for volatile object
   * @param tag user defined marker/tag
   * @param persist whether to write the token back to storage medium immediately
   * */
  void hire(bool mode, uint8_t tag, bool persist) {
    assert(token_ == kFreeCode && tag <= kMarkBits);
    uint8_t user = (mode ? kModeBit : 0) | tag;
    token_ = kInuseBit | user;
    if(kEnhancedADR) persist_wait_finish();
    else if(persist) wait_write_back(this, sizeof(token_t));
  }

  /**
   * @brief to fire corresponding region, mark it as free
   * or to init a region as free/unused state
   * @param persist whether to write token back to storage medium immediately
   * */
  void fire(bool persist = true) {
    if(token_ != kFreeCode) {
      token_.store(kFreeCode, store_order);
      if(kEnhancedADR) persist_wait_finish();
      else if(persist) wait_write_back(this, sizeof(token_t));
    }
  }

  /**
   * @brief whether the region is being used
   * */
  bool busy() const { return token_.load(load_order) & kInuseBit; }

  /**
   * @brief the use mode of corresponding memory object, true for persistent object, false for volatile object
   * */
  bool mode() const { return token_.load(load_order) & kModeBit; }

  /**
   * @brief user-defined marker
   * */
  uint8_t marker() const { return token_.load(load_order) & kMarkBits; }
}; // memory object persistent token/marker type

static_assert(sizeof(token_t) == 1);

}

#endif //SLABSTORE_TOKEN_H
