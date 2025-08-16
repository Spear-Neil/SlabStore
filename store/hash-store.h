/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_HASH_STORE_H
#define SLABSTORE_HASH_STORE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <array>
#include <atomic>

#include "kv-type.h"
#include "hash-table.h"
#include "../slab/slab.h"

namespace SlabStore {

using util::RowValue;
using util::OptRowValue;

struct HashConfig : DefaultHashConfig {
  static constexpr bool kInMemStore = false;
  static constexpr bool kOptUpdate = true;
};

template<typename K, typename V>
class HashStore {
  HashTable<K, V, HashConfig> index_;
  Allocator slab_;
  size_t size_;  // version table size
  std::atomic<uint64_t>* version_; // version table

  static_assert(std::is_same<V, OptRowValue>());
 public:
  typedef util::KVPair<K, V> KVPair;

  /**
   * @brief constructor
   * @param size version table size
   * */
  HashStore(size_t size = 128) : index_(), slab_(), version_(nullptr) {
    version_ = new std::atomic<uint64_t>[size]{};
    for(size_t i = 0; i < size; i++) version_[i] = 0;
  }

  ~HashStore() { delete[] version_; }

  /**
   * @brief create a store or open an existing store (including recovery)
   * @param path store path
   * @param nthd thread number used for recovery
   * @param nid numa node index to which recovery threads are pinned
   * */
  void open(const std::string& path, size_t nthd = 1, size_t nid = 0) {
    slab_.open(path, -1, nid);

  }

  /**
   * @brief insert a key-value pair into the store
   * @return true for insert succeed, false means that the insert operation
   * overrides an existing key-value pair, perhaps leading to inconsistency.
   * */
  bool insert(const K& key, void* value, int vlen) {

  }

  /**
   * @brief update an existing key-value pair
   * @brief true for update succeed, false means the key-value pair does not exist.
   * */
  bool update(const K& key, void* value, int vlen) {

  }

  /**
   * @brief lookup for corresponding key-value pair
   * */
  KVPair* lookup(const K& key) {

  }
};

}

#endif //SLABSTORE_HASH_STORE_H
