/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_HASH_STORE_H
#define SLABSTORE_HASH_STORE_H

#include <cstddef>
#include <cstdint>

#include "hash-table.h"
#include "../slab/slab.h"

namespace SlabStore {

template<typename K, typename V>
class HashStore {
  HashTable<K, V> index_;
  Allocator slab_;

 public:


};

}

#endif //SLABSTORE_HASH_STORE_H
