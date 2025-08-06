/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_HASH_STORE_H
#define SLABSTORE_HASH_STORE_H

#include <cstddef>
#include <cstdint>

#include "kv-type.h"
#include "hash-table.h"
#include "../slab/slab.h"

namespace SlabStore {

using util::RowValue;
using util::OptRowValue;

template<typename K, typename V>
class HashStore {
  HashTable<K, V> index_;
  Allocator slab_;

  static_assert(std::is_same<V, RowValue>() || std::is_same<V, OptRowValue>());
 public:
  typedef util::KVPair<K, V> KVPair;

  void upsert() {

  }

  KVPair* lookup(){

  }
};

}

#endif //SLABSTORE_HASH_STORE_H
