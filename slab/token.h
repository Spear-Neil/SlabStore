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
#include <tuple>

#include "persist.h"

namespace SlabStore {

class token_t {
  std::atomic<uint8_t> token_;
  // Layout of token: | inuse (1bit) | mode (1bit)         | option (1bit)      | marker (5bits) |
  // assignment:      | free/busy    | volatile/persistent | default/customized | user-defined   |
  // The first significant bit is used as occupation marker; zero for free, one for being used;
  // The second significant bit is used as use mode marker; zero for use the persistent memory
  // object as volatile memory object (when system crashes [StatCode: kVolatile], all contents
  // in the memory object is view as damaged or invalid, so when rebuilding allocator's meta-info,
  // such memory object is marked as free; after normal system shutdown, data in such objects can
  // be viewed as consistent); one for persistent memory object (user is in charge of maintaining
  // data consistency through a programming model similar to reserve-publish, after system crashes
  // due to power failure, we presume that data in such persistent memory object is consistent)
  // The third significant bit is used as an option indicating whether it has user-defined marker,
  // zero for default object without user-defined marker, one for memory object with user-defined
  // marker (both volatile and persistent memory object can be attached with a marker);
  // The remaining five bits are used as user-defined marker (e.g. classification tags for different
  // customized memory objects, 0 - 31)

  static constexpr uint8_t kInuseBit = 0x80;
  static constexpr uint8_t kModeBit = 0x40;
  static constexpr uint8_t kOptionBit = 0x20;
  static constexpr uint8_t kMarkBits = 0x1F;

  // the region is free/unused
  static constexpr uint8_t kFreeCode = 0x00;

  // ensure every load/store operation is compiled as memory operation
  // data consistency and memory ordering is maintaining by upper level logic
  static constexpr std::memory_order load_order = std::memory_order_relaxed;
  static constexpr std::memory_order store_order = std::memory_order_relaxed;

 public:
  token_t() = delete;

  ~token_t() = default;

  token_t(const token_t&) = delete;

  token_t& operator=(const token_t&) = delete;

  /**
   * @brief to init a region as free/unused state
   * */
  void init(bool persist = true) {
    if(token_ != kFreeCode) {
      token_.store(kFreeCode, store_order);
      if(persist) {
        persist_write_back(this, sizeof(token_t));
        persist_wait_finish();
      }
    }
  }

  /**
   * @brief to hire corresponding region
   * */
  void hire(bool persist = true) {
    if(persist) {
      persist_write_back(this, sizeof(token_t));
      persist_wait_finish();
    }
  }

  /**
   * @brief to fire corresponding region, mark it as free
   * */
  void fire(bool persist = true) {
    if(token_ != kFreeCode) {
      token_.store(kFreeCode, store_order);
      if(persist) {
        persist_write_back(this, sizeof(token_t));
        persist_wait_finish();
      }
    }
  }

  /**
   * @brief whether the region is being used
   * */
  bool busy() { return token_.load(load_order) & kInuseBit; }

  /**
   * @brief the use mode of corresponding memory object
   * */
  bool mode() { return token_.load(load_order) & kModeBit; }

  /**
   * @brief whether the region is attached with user-defined mark
   * */
  bool customized() { return token_.load(load_order) & kOptionBit; }

  /**
   * @brief user-defined marker
   * */
  uint8_t marker() { return token_.load(load_order) & kMarkBits; }
}; // memory object persistent token/marker type

static_assert(sizeof(token_t) == 1);


typedef std::pair<token_t*, void*> region_t; // region type, start address of a region and its token

}

#endif //SLABSTORE_TOKEN_H
