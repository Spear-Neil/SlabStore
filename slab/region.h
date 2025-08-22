/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_REGION_H
#define SLABSTORE_REGION_H

#include <tuple>
#include "token.h"

namespace SlabStore {

class region_t {
  std::pair<token_t*, void*> pair_; // start address of a region and its corresponding token

 public:
  region_t(token_t* token, void* region) : pair_(token, region) {}

  region_t(const region_t&) = default;

  ~region_t() = default;

  region_t& operator=(const region_t&) = default;

  operator void*() const { return pointer(); }

  /**
   * @brief raw container of regions
   * */
  std::pair<token_t*, void*> raw() const { return pair_; }

  /**
   * @brief start address of the region
   * */
  void* pointer() const { return pair_.second; }

  /**
   * @brief set_control an nvm object with its use mode and user-defined tag
   * @param mode use mode, true for persistent object, false for volatile object
   * @param tag user-defined marker/tag (zero by default), [0, 63]
   * @param persist whether to write the token back to storage medium immediately
   * */
  void publish(bool mode = true, uint8_t tag = 0, bool persist = true) const {
    pair_.first->hire(mode, tag, persist);
  }

  /**
   * @brief whether the regions is being used
   * */
  bool busy() const { return pair_.first->busy(); }

  /**
   * @brief the use mode of corresponding memory object, true for persistent object, false for volatile object
   * */
  bool mode() const { return pair_.first->mode(); }

  /**
   * @brief user-defined marker
   * */
  uint8_t marker() const { return pair_.first->marker(); }
};

}

#endif //SLABSTORE_REGION_H
