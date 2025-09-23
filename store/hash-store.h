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
#include <unordered_set>
#include <iostream>

#include "timer.h"
#include "hash-table.h"
#include "kv-type.h"
#include "../slab/slab.h"

namespace SlabStore {

using util::Timer;
using util::OptRowValue;
using util::String;
using util::Epoch;
using util::DensePointer;
using SlabStore::internal::hash_code;

struct HashStoreConfig : DefaultHashConfig {
  static constexpr bool kInMemStore = false;
  static constexpr bool kOptUpdate = true;
  static constexpr bool kTimeStamp = true;

  static constexpr bool kLogInfo = true;  // whether to print log information
  static constexpr size_t kReclaimTomb = 0x01ul << 30; // space threshold for reclaim tombstone, 1GB
};

template<typename K, typename StoreConfig = HashStoreConfig, size_t N = 256>
class HashStore {
  typedef SlabStore::HashTable<K, OptRowValue, StoreConfig> HashTable;
  typedef typename HashTable::Segment Segment;
  typedef typename HashTable::Bucket Bucket;

  Allocator slab_;
  HashTable index_;
  std::atomic<size_t> tomb_size_;  // space occupied by tombstone
  std::atomic<uint64_t>* version_; // version table

  static constexpr size_t kNSlot = StoreConfig::kNSlotInBucket;
  static constexpr size_t kNBucket = StoreConfig::kNBucketInSegment;
  static constexpr size_t kReclaimTomb = StoreConfig::kReclaimTomb;
  static constexpr bool kLogInfo = StoreConfig::kLogInfo;

  // meta-information on persistent memory for fast reboot
  struct PersistHead {
    size_t depth; // global depth of hash table
    size_t tomb_size; // space occupied by tombstone
    pptr64_t dir; // directory of hash table
    pptr64_t ver; // version table
  };

  // hash bucket residing on persistent memory for fast reboot
  struct PersistBucket {
    uint64_t bitmap: 48;
    uint8_t depth;
    uint8_t index;
    uint8_t bucket_tags[kNSlot];
    uint16_t slot_tags[kNSlot];
    pptr64_t kvs[kNSlot];
  };

 public:
  typedef util::KVPair<K, OptRowValue> KVPair;

  HashStore() : slab_(), index_(), tomb_size_(0), version_(nullptr) {
    version_ = new std::atomic<uint64_t>[N]{};
    for(size_t i = 0; i < N; i++) version_[i] = 0;
  }

  ~HashStore() {
    // unload in-memory structure to persistent memory for fast reboot
    auto head = slab_.acquire(sizeof(PersistHead));
    auto pdir = slab_.acquire(sizeof(pptr64_t) * index_.directory_size());
    auto pver = slab_.acquire(sizeof(uint64_t) * N);

    bool reclaim = tomb_size_ > kReclaimTomb; // whether to reclaim tombstone
    if(reclaim) tomb_size_ = 0;

    auto ph = (PersistHead*) head.pointer();
    ph->depth = index_.depth();
    ph->tomb_size = tomb_size_;
    ph->dir.store(pdir.pointer());
    ph->ver.store(pver.pointer());
    persist_write_back(ph, sizeof(PersistHead));
    head.publish(false), pdir.publish(false), pver.publish(false);

    slab_.root()[0].store(ph);
    persist_write_back(&slab_.root()[0], sizeof(slab_.root()[0]));
    memcpy(pver.pointer(), version_, sizeof(uint64_t) * N);
    persist_write_back(pver.pointer(), sizeof(uint64_t) * N);

    index_.get_epoch().clear(); // release all older kvs before reclaim tombstone
    size_t count = index_.directory_size();

    for(size_t sid = 0; sid < count; sid++) { // unload all segments
      Segment* vsegment = index_.segment(sid);
      size_t depth = vsegment->depth();

      if(sid < (0x01ul << depth)) {
        auto psegment = slab_.acquire(sizeof(PersistBucket) * kNBucket);

        for(size_t bid = 0; bid < kNBucket; bid++) { // unload all buckets in a segment
          auto& pb = ((PersistBucket*) psegment)[bid];
          Bucket* vb = vsegment->bucket(bid);

          pb.bitmap = vb->bitmap();
          pb.depth = vb->depth();
          pb.index = vb->index();
          for(size_t tid = 0; tid < kNSlot; tid++) {
            pb.bucket_tags[tid] = vb->tags(tid);
            DensePointer kv = vb->kvs(tid);
            pb.slot_tags[tid] = kv.remain();
            pb.kvs[tid].store(kv.pointer());

            if(reclaim && (vb->bitmap() & (0x01ul << tid)) &&
               ((KVPair*) kv.pointer())->tombstone()) { // reclaim tombstone
              slab_.release(kv.pointer());
              pb.bitmap &= ~(0x01ul << tid);
            }
          }
        }

        persist_write_back(psegment, sizeof(PersistBucket) * kNBucket);
        psegment.publish(false);
        ((pptr64_t*) pdir)[sid].store(psegment.pointer());
      } else { // previously unloaded segment
        size_t index = sid % (0x01ul << depth);
        ((pptr64_t*) pdir)[sid].store(((pptr64_t*) pdir)[index].load());
      }
    }
    wait_write_back(pdir, sizeof(pptr64_t) * index_.directory_size());

    delete[] version_;
  }

  Epoch& get_epoch() { return index_.get_epoch(); }

  /**
   * @brief create a store or open an existing store (including recovery)
   * @param path store path
   * @param size store size in bytes
   * @param nthd thread number used for recovery
   * @param nid numa node index to which recovery threads are pinned
   * */
  void open(const std::string& path, size_t size = -1, size_t nthd = 1, size_t nid = 0) {
    Timer timer;
    timer.start();
    slab_.open(path, size);
    long drt = timer.duration_us();
    if(kLogInfo) std::cout << "[HashStore]: slab allocator open elapsed time: " << drt << " microseconds" << std::endl;
    if(!slab_.good()) { // recover from power failure or system crashes
      if(kLogInfo) std::cout << "[HashStore]: recover from abnormal crashes" << std::endl;
      timer.start();
      slab_.recover([&](region_t obj) {
        if(obj.mode()) { // kv object
          uint64_t code = hash_code(((KVPair*) obj.pointer())->key);
          KVPair* res = index_.upsert((KVPair*) obj.pointer(), code);
          if(((KVPair*) obj.pointer())->tombstone())
            tomb_size_.fetch_add(slab_.region_size(obj));

          if(res == (KVPair*) internal::kExpired) {
            slab_.release(obj.pointer());
            return;
          } // an expired version

          // insert/update the latest version
          if(res != (KVPair*) internal::kNotFound) { // has an older version
            index_.get_epoch().retire([&, res]() { slab_.release(res); });
          }

          // update version table
          uint64_t latest = ((KVPair*) obj.pointer())->load_version();
          uint64_t origin = version_[code % N].load(std::memory_order_acquire);
          while(latest > origin && !version_[code % N].compare_exchange_strong(origin, latest));
        } else { // auxiliary structure for fast reboot, reclaim immediately
          slab_.release(obj.pointer());
        }
      }, nthd, nid);
      long rdrt = timer.duration_us();
      if(kLogInfo) {
        std::cout << "[HashStore]: recover elapsed time: " << rdrt << " microseconds" << std::endl;
        std::cout << "[HashStore]: total open/recover elapsed time: " << drt + rdrt << " microseconds" << std::endl;
      }
    } else { // fast reboot from normal shutdown
      if(kLogInfo) std::cout << "[HashStore]: fast reboot from normal shutdown" << std::endl;
      Timer timer;
      timer.start();
      PersistRoot& root = slab_.root();
      if(root[0].load() != nullptr) {
        auto head = (PersistHead*) root[0].load();
        size_t count = 0x01ul << head->depth; // directory entry count
        tomb_size_ = head->tomb_size;
        auto pdir = (pptr64_t*) head->dir.load();
        auto pver = (std::atomic<uint64_t>*) head->ver.load();
        memcpy(version_, pver, sizeof(uint64_t) * N);

        Segment** vdir = new Segment* [count]{};
        DensePointer* kvs = new DensePointer[kNSlot]{};
        uint8_t* tags = new uint8_t[kNSlot]{};

        // reload all segments
        for(size_t sid = 0; sid < count; sid++) {
          auto psegment = (PersistBucket*) pdir[sid].load();
          size_t depth = psegment->depth; // local depth

          if(sid < (0x01ul << depth)) {
            Segment* vsegment = new Segment(depth);

            for(size_t bid = 0; bid < kNBucket; bid++) { // reload all buckets in a segment
              PersistBucket& pb = psegment[bid];
              Bucket* vb = vsegment->bucket(bid);
              for(size_t tid = 0; tid < kNSlot; tid++) {
                void* kv = pb.kvs[tid].load();
                uint16_t tag = pb.slot_tags[tid];
                kvs[tid] = DensePointer(kv, tag);
              }
              memcpy(tags, pb.bucket_tags, sizeof(uint8_t) * kNSlot);
              new(vb) Bucket(pb.bitmap, pb.depth, pb.index, tags, kvs);
            }

            vdir[sid] = vsegment;
          } else { vdir[sid] = vdir[sid % (0x01ul << depth)]; }
        }

        // release all persistent buckets
        std::unordered_set<void*> released;
        for(size_t sid = 0; sid < count; sid++) {
          auto psegment = (PersistBucket*) pdir[sid].load();
          if(released.find(psegment) == released.end()) {
            slab_.release(psegment);
            released.insert(psegment);
          }
        }

        HashTable table(head->depth, vdir);
        index_.swap(table);

        delete[] tags, delete[] kvs, delete[] vdir;
        slab_.release(pdir), slab_.release(pver), slab_.release(head);
      }
      long rdrt = timer.duration_us();
      if(kLogInfo) {
        std::cout << "[HashStore]: reboot elapsed time: " << rdrt << " microseconds" << std::endl;
        std::cout << "[HashStore]: total open/recover elapsed time: " << drt + rdrt << " microseconds" << std::endl;
      }
    }
    slab_.root()[0].store(nullptr);
    wait_write_back(&slab_.root()[0], sizeof(slab_.root()[0]));
  }

  /**
   * @brief insert or update a key-value pair into the store
   * @return true for insert, false for update
   * */
  bool upsert(const K& key, void* value, int vlen) {
    uint64_t code = hash_code(key), kv_len = 0;
    uint64_t version = version_[code % N]++;
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
    uint64_t version = version_[code % N]++;
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

    if(kv == (KVPair*) internal::kNotFound)
      return (KVPair*) internal::kNotFound;

    if(!kv->tombstone()) return kv;
    return (KVPair*) internal::kNotFound;
  }

  /**
   * @brief delete an existing kv corresponding to the key
   * @return true for remove succeed, false means the key-value pair does not exist
   * */
  bool remove(const K& key) {
    uint64_t code = hash_code(key), kv_len = 0;
    uint64_t version = version_[code % N]++;
    if constexpr(std::is_same_v<K, String>) {
      kv_len = sizeof(KVPair) + key.len;
    } else { kv_len = sizeof(KVPair); }

    // a previous old kv version may have not released into allocator by epoch-based reclaimer,
    // so we insert a tombstone for delete operation, and its space will be reclaimed during store closing
    auto kv = slab_.acquire(kv_len);
    tomb_size_.fetch_add(slab_.region_size(kv));
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

  /**
   * @brief number of kv pairs (including tombstone), thread-unsafe
   * */
  size_t size() { return index_.size(); }
};

}

#endif //SLABSTORE_HASH_STORE_H
