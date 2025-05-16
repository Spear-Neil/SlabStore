/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_PPTR_H
#define SLABSTORE_PPTR_H

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <atomic>

namespace SlabStore {

namespace internal {

/** atomic persistent pointer on non-volatile memory */
template<typename off_t>
class pptr_t {
  std::atomic<off_t> off_;

  // the smallest negative value as invalid offset
  static constexpr int kShift = sizeof(off_t) * 8 - 1;
  static constexpr off_t kInvalid = off_t(0x01) << kShift;
  // make sure off_t is signed int type, so short int type can be signed-extended
  static_assert(std::is_signed<off_t>() && std::is_integral<off_t>());

  // we do not guarantee memory ordering, only guarantee every operation to
  // pptr is compiled as memory access operation not optimized as register
  static constexpr std::memory_order load_order = std::memory_order_relaxed;
  static constexpr std::memory_order store_order = std::memory_order_relaxed;

 public:
  pptr_t() : off_(kInvalid) {}

  ~pptr_t() = default;

  explicit pptr_t(const void* ptr) { store(ptr); }

  pptr_t& operator=(const void* ptr) {
    store(ptr);
    return *this;
  }

  pptr_t(const pptr_t& pptr) { store(pptr.load()); }

  pptr_t& operator=(const pptr_t& pptr) {
    store(pptr.load());
    return *this;
  }

  void* load(std::memory_order order = load_order) const {
    off_t off = off_.load(order);
    if(off == kInvalid) return nullptr;
    return (void*) ((size_t) this + off);
  }

  void store(const void* ptr, std::memory_order order = store_order) {
    off_t off = kInvalid;
    if(ptr != nullptr) { off = (size_t) ptr - (size_t) this; }
    off_.store(off, order);
  }

  operator void*() const { return load(); }
};

}

typedef internal::pptr_t<int32_t> pptr32_t;
typedef internal::pptr_t<int64_t> pptr64_t;

}

#endif //SLABSTORE_PPTR_H
