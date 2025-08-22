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

#include "hash-table.h"
#include "kv-type.h"
#include "../slab/slab.h"

namespace SlabStore {

using util::OptRowValue;
using util::String;
using util::Epoch;
using SlabStore::internal::hash_code;

struct HashStoreConfig : DefaultHashConfig {
  static constexpr bool kInMemStore = false;
  static constexpr bool kOptUpdate = true;
  static constexpr bool kTimeStamp = true;
};

template<typename K, size_t size = 256>
class HashStore {
  Allocator slab_;
  HashTable<K, OptRowValue, HashStoreConfig> index_;
  std::atomic<uint64_t>* version_; // version table

 public:
  typedef util::KVPair<K, OptRowValue> KVPair;

  HashStore() : slab_(), index_(), version_(nullptr) {
    version_ = new std::atomic<uint64_t>[size]{};
    for(size_t i = 0; i < size; i++) version_[i] = 0;
  }

  ~HashStore() {
    // offload in-memory structure to persistent memory for fast reboot

    delete[] version_;
  }

  Epoch& get_epoch() { return index_.get_epoch(); }

  /**
   * @brief create a store or open an existing store (including recovery)
   * @param path store path
   * @param nthd thread number used for recovery
   * @param nid numa node index to which recovery threads are pinned
   * */
  void open(const std::string& path, size_t nthd = 1, size_t nid = 0) {
    slab_.open(path, -1, nid);
    if(!slab_.good()) { // recover from power failure or system crashes
      slab_.recover([&](region_t obj) {
        if(obj.mode()) { // kv object
          KVPair* res = index_.upsert((KVPair*) obj.pointer());
          if(res == (KVPair*) internal::kNotFound) return; // insert the latest version at present
          if(res == (KVPair*) internal::kExpired) { slab_.release(obj.pointer()); } // an expired version
          else { index_.get_epoch().retire([&, res]() { slab_.release(res); }); } // update the latest
        } else { // auxiliary structure for fast reboot, reclaim immediately
          slab_.release(obj.pointer());
        }
      }, nthd, nid);
    } else { // fast reboot from normal shutdown
      PersistRoot& root = slab_.root();
      if(root[0].load() != nullptr) {

      }
    }
  }

  /**
   * @brief insert or update a key-value pair into the store
   * @return true for insert, false for update
   * */
  bool upsert(const K& key, void* value, int vlen) {
    uint64_t code = hash_code(key), kv_len = 0;
    uint64_t version = version_[code % size]++;
    if constexpr(std::is_same_v<K, String>) {
      kv_len = sizeof(KVPair) + key.len + vlen;
    } else { kv_len = sizeof(KVPair) + vlen; }

    auto kv = slab_.acquire(kv_len);
    KVPair::make_kv((KVPair*) kv.pointer(), key, value, vlen);
    ((KVPair*) kv.pointer())->set_control(version, false);
    wait_write_back(kv.pointer(), kv_len);
    kv.publish(true, 0, true);

    KVPair* old = index_.upsert((KVPair*) kv.pointer(), code);

    if(old == (KVPair*) internal::kNotFound) return true; // insertion succeeds

    // no any other threads are referencing this, release immediately
    if(old == (KVPair*) internal::kExpired) { slab_.release(kv.pointer()); }
    else { index_.get_epoch().retire([&, old]() { slab_.release(old); }); }
    return false; // update
  }

  /**
   * @brief update an existing key-value pair
   * @return true for update succeed, false means the key-value pair does not exist.
   * */
  bool update(const K& key, void* value, int vlen) {
    uint64_t code = hash_code(key), kv_len = 0;
    uint64_t version = version_[code % size]++;
    if constexpr(std::is_same_v<K, String>) {
      kv_len = sizeof(KVPair) + key.len + vlen;
    } else { kv_len = sizeof(KVPair) + vlen; }

    auto kv = slab_.acquire(kv_len);
    KVPair::make_kv((KVPair*) kv.pointer(), key, value, vlen);
    ((KVPair*) kv.pointer())->set_control(version, false);
    wait_write_back(kv.pointer(), kv_len);
    kv.publish(true, 0, true);

    KVPair* old = index_.update((KVPair*) kv.pointer(), code);

    if(old == (KVPair*) internal::kNotFound) { // try to update a nonexisting kv
      slab_.release(kv.pointer());
      return false;
    }

    // update succeed, but kv is expired
    if(old == (KVPair*) internal::kExpired) { slab_.release(kv.pointer()); }
    else { index_.get_epoch().retire([&, old]() { slab_.release(old); }); }
    return true;
  }

  /**
   * @brief lookup for corresponding key-value pair
   * */
  KVPair* lookup(const K& key) {
    KVPair* kv = index_.lookup(key);
    if(!kv->tombstone()) return kv;
    return (KVPair*) internal::kNotFound;
  }

  /**
   * @brief delete an existing kv corresponding to the key
   * @return true for remove succeed, false means the key-value pair does not exist
   * */
  bool remove(const K& key) {
    uint64_t code = hash_code(key), kv_len = 0;
    uint64_t version = version_[code % size]++;
    if constexpr(std::is_same_v<K, String>) {
      kv_len = sizeof(KVPair) + key.len;
    } else { kv_len = sizeof(KVPair); }

    // a previous old kv version may have not released into allocator by epoch-based reclaimer,
    // so we insert a tombstone for delete operation, and its space will be reclaimed during store closing
    auto kv = slab_.acquire(kv_len);
    KVPair::make_kv((KVPair*) kv.pointer(), key, nullptr, 0);
    ((KVPair*) kv.pointer())->set_control(version, true); // tombstone
    wait_write_back(kv.pointer(), kv_len);
    kv.publish(true, 0, true);

    KVPair* old = index_.upsert((KVPair*) kv.pointer(), code);

    if(old == (KVPair*) internal::kNotFound) { // try to delete a non-existing kv
      slab_.release(kv.pointer());
      return false;
    }

    // note: if another thread's operation is update, it actually does insertion
    // cause logically the kv pair has deleted by current operation
    if(old == (KVPair*) internal::kExpired) { slab_.release(kv.pointer()); }
    else { index_.get_epoch().retire([&, old]() { slab_.release(old); }); }
    return true;
  }
};

}

#endif //SLABSTORE_HASH_STORE_H
