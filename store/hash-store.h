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
#include <array>
#include <deque>
#include <vector>
#include <thread>
#include <unordered_map>

#include <sys/time.h>
#include <sys/resource.h>

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

  static constexpr bool kWriteOpt = true; // write-optimized update operation
  static constexpr size_t kWrtOptMaxSize = 2048; // max record size in write-optimized update
  static constexpr size_t kMaxLimboSize = 1024;  // max number of expired kv pairs in ThreadLimbo

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
  SizeClass* sc_;
  std::atomic<size_t> tomb_size_;  // space occupied by tombstone
  std::atomic<uint64_t>* version_; // version table

  static constexpr size_t kNSlot = StoreConfig::kNSlotInBucket;
  static constexpr size_t kNBucket = StoreConfig::kNBucketInSegment;
  static constexpr size_t kReclaimTomb = StoreConfig::kReclaimTomb;
  static constexpr bool kLogInfo = StoreConfig::kLogInfo;

  static constexpr bool kEnhancedADR = SlabConst::kEnhancedADR;
  static constexpr bool kWriteOpt = StoreConfig::kWriteOpt;
  static constexpr size_t kWrtOptMaxSize = StoreConfig::kWrtOptMaxSize;
  static constexpr size_t kMaxLimboSize = StoreConfig::kMaxLimboSize;
  static constexpr size_t kLimboCount = SizeClass::size2index_compute(kWrtOptMaxSize) + 1;

  static_assert(kWrtOptMaxSize == SizeClass::index2size_compute(kLimboCount - 1));

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

  // kv pairs whose version is expired but haven't been back released to allocator
  class ThreadLimbo {
    Allocator* slab_;
    SizeClass* sc_;
    std::array<std::deque<void*>, kLimboCount> lists_;

   public:
    ThreadLimbo(Allocator* slab, SizeClass* sc) : slab_(slab), sc_(sc) {}

    ~ThreadLimbo() {
      for(const auto& list : lists_) {
        for(void* ptr : list) slab_->release(ptr);
      }
    }

    // put expired objects into limbo lists or release it back to allocator
    void release(void* ptr) {
      assert(ptr != nullptr);
      size_t len = slab_->region_size(ptr);
      size_t ind = sc_->size2index(len);
      if(ind < kLimboCount &&
         lists_[ind].size() < kMaxLimboSize) {
        lists_[ind].push_back(ptr);
      } else { slab_->release(ptr); }
    }

    // acquire an expired object from limbo list
    void* acquire(size_t size) {
      size_t ind = sc_->size2index(size);
      if(ind < kLimboCount && !lists_[ind].empty()) {
        void* ptr = lists_[ind].front();
        lists_[ind].pop_front();
        return ptr;
      } else { return nullptr; }
    }
  };

  typedef tbb::concurrent_unordered_map<std::thread::id, ThreadLimbo*, std::hash<std::thread::id>> tsd_t;
  tsd_t tsd_; // thread limbo lists

  ThreadLimbo& thread_limbo() {
    static thread_local struct {
      void* pointer = nullptr; // raw pointer of allocator
      ThreadLimbo* limbo = nullptr;
    } local;

    // switch between different stores
    if(branch_unlikely(local.pointer != slab_.pointer())) {
      local.pointer = slab_.pointer();

      auto tid = std::this_thread::get_id();
      auto it = tsd_.find(tid);
      if(it == tsd_.end()) { // lazily create thread limbo list
        local.limbo = new ThreadLimbo(&slab_, sc_);
        tsd_.insert({tid, local.limbo});
      } else { local.limbo = it->second; }
    }

    return *local.limbo;
  }

  bool same_cache_line(void* ptr, size_t len) {
    ptrdiff_t line0 = (ptrdiff_t) ptr & ~(kCacheLineSize - 1);
    ptrdiff_t line1 = ((ptrdiff_t) ptr + len) & ~(kCacheLineSize - 1);
    return line0 == line1;
  }

 public:
  typedef util::KVPair<K, OptRowValue> KVPair;

  HashStore() : slab_(), index_(), sc_(nullptr), tomb_size_(0), version_(nullptr) {
    /*if(kWriteOpt)*/ sc_ = new SizeClass();
    version_ = new std::atomic<uint64_t>[N]{};
    for(size_t i = 0; i < N; i++) version_[i] = 0;
  }

  ~HashStore() {
    // destroy thread local limbo list first
    for(auto accessor : tsd_) { delete accessor.second; }
    // unload in-memory structure to persistent memory for fast reboot
    size_t dir_size = sizeof(pptr64_t) * index_.directory_size();
    // max size 4MB, split directory into multiple blocks if larger than 4MiB
    if(dir_size >= 4 * 1024 * 1024) {
      std::cerr << "Currently, not supported directory size larger than 4MiB" << std::endl;
      exit(-1);
    }
    auto head = slab_.acquire(sizeof(PersistHead));
    auto pdir = slab_.acquire(dir_size);
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
    if(kWriteOpt) delete sc_;
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
      struct rusage before{}, after{};
      int res = getrusage(RUSAGE_SELF, &before);
      if(res != 0) {
        std::cerr << "[ERROR]: getrusage unknown error!" << std::endl;
        exit(-1);
      }
      timer.start();
      slab_.recover([&](region_t obj) {
        if(obj.mode()) { // kv object
          KVPair* kv = (KVPair*) obj.pointer();
          if(kWriteOpt && kv->invalid()) {
            slab_.release(kv); // invalid kv pairs
            return;
          }

          uint64_t code = hash_code(kv->key);
          KVPair* res = index_.upsert(kv, code);
          if(kv->tombstone())
            tomb_size_.fetch_add(slab_.region_size(kv));

          if(res == (KVPair*) internal::kExpired) {
            slab_.release(obj.pointer());
            return;
          } // an expired version

          // insert/update the latest version
          if(res != (KVPair*) internal::kNotFound) { // has an older version
            index_.get_epoch().retire([&, res]() { slab_.release(res); });
          }

          // update version table
          uint64_t latest = kv->load_version();
          uint64_t origin = version_[code % N].load(std::memory_order_acquire);
          while(latest > origin && !version_[code % N].compare_exchange_strong(origin, latest));
        } else { // auxiliary structure for fast reboot, reclaim immediately
          slab_.release(obj.pointer());
        }
      }, nthd, nid);
      long rdrt = timer.duration_us();
      res = getrusage(RUSAGE_SELF, &after);
      if(res != 0) {
        std::cerr << "[ERROR]: getrusage unknown error!" << std::endl;
        exit(-1);
      }
      if(kLogInfo) {
        size_t user_time = after.ru_utime.tv_sec * 1'000'000 + after.ru_utime.tv_usec;
        user_time -= before.ru_utime.tv_sec * 1'000'000 + before.ru_utime.tv_usec;
        size_t sys_time = after.ru_stime.tv_sec * 1'000'000 + after.ru_stime.tv_usec;
        sys_time -= before.ru_stime.tv_sec * 1'000'000 + before.ru_stime.tv_usec;
        std::cout << "[HashStore]: recover total user CPU time: " << user_time << " microseconds" << std::endl;
        std::cout << "[HashStore]: recover total sys CPU time: " << sys_time << " microseconds" << std::endl;
        std::cout << "[HashStore]: recover elapsed real time: " << rdrt << " microseconds" << std::endl;
        std::cout << "[HashStore]: total open/recover elapsed real time: " << drt + rdrt << " microseconds"
                  << std::endl;
      }
    } else { // fast reboot from normal shutdown
      if(kLogInfo) std::cout << "[HashStore]: fast reboot from normal shutdown" << std::endl;
      PinningMap pin;
      pin.reset_pinning_counter(nid, 0);
      struct rusage before{}, after{};
      int res = getrusage(RUSAGE_SELF, &before);
      if(res != 0) {
        std::cerr << "[ERROR]: getrusage unknown error!" << std::endl;
        exit(-1);
      }
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

        // reload all segments in parallel
        std::vector<std::thread> workers;
        std::atomic<size_t> loaded{0};
        std::atomic<size_t> finished{0};
        for(int tid = 0; tid < nthd; tid++) {
          workers.push_back(std::thread([&](int tid) {
            pin.pinning_thread_continuous(pthread_self());
            DensePointer* kvs = new DensePointer[kNSlot]{};
            uint8_t* tags = new uint8_t[kNSlot]{};
            std::deque<void*> psegments;
            std::unordered_map<Segment**, Segment**> sharing;

            while(true) {
              size_t sid = loaded.fetch_add(1);
              if(sid >= count) break;

              auto psegment = (PersistBucket*) pdir[sid].load();
              size_t depth = psegment->depth; // local depth

              if(sid < (0x01ul << depth)) {
                psegments.push_back(psegment);
                Segment* vsegment = new Segment(depth);
                for(size_t bid = 0; bid < kNBucket; bid++) { // reload all buckets in a segment
                  PersistBucket& pb = psegment[bid];
                  Bucket* vb = vsegment->bucket(bid);
                  for(size_t rid = 0; rid < kNSlot; rid++) {
                    void* kv = pb.kvs[rid].load();
                    uint16_t tag = pb.slot_tags[rid];
                    kvs[rid] = DensePointer(kv, tag);
                  }
                  memcpy(tags, pb.bucket_tags, sizeof(uint8_t) * kNSlot);
                  new(vb) Bucket(pb.bitmap, pb.depth, pb.index, tags, kvs);
                }
                vdir[sid] = vsegment;
              } else { sharing[&vdir[sid]] = &vdir[sid % (0x01ul << depth)]; }
            }
            // wait for other threads
            finished.fetch_add(1);
            while(finished.load(std::memory_order_acquire) != nthd);

            for(auto pair : sharing) { *pair.first = *pair.second; }
            for(void* pseg : psegments) { slab_.release(pseg); }
            delete[] tags, delete[] kvs;
          }, tid));
        }
        for(int tid = 0; tid < nthd; tid++) { workers[tid].join(); }

/*
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
*/

        HashTable table(head->depth, vdir);
        index_.swap(table);

//        delete[] tags, delete[] kvs;
        delete[] vdir;
        slab_.release(pdir), slab_.release(pver), slab_.release(head);
      }
      long rdrt = timer.duration_us();
      res = getrusage(RUSAGE_SELF, &after);
      if(res != 0) {
        std::cerr << "[ERROR]: getrusage unknown error!" << std::endl;
        exit(-1);
      }
      if(kLogInfo) {
        size_t user_time = after.ru_utime.tv_sec * 1'000'000 + after.ru_utime.tv_usec;
        user_time -= before.ru_utime.tv_sec * 1'000'000 + before.ru_utime.tv_usec;
        size_t sys_time = after.ru_stime.tv_sec * 1'000'000 + after.ru_stime.tv_usec;
        sys_time -= before.ru_stime.tv_sec * 1'000'000 + before.ru_stime.tv_usec;
        std::cout << "[HashStore]: reboot total user CPU time: " << user_time << " microseconds" << std::endl;
        std::cout << "[HashStore]: reboot total sys CPU time: " << sys_time << " microseconds" << std::endl;
        std::cout << "[HashStore]: reboot elapsed real time: " << rdrt << " microseconds" << std::endl;
        std::cout << "[HashStore]: total open/recover elapsed real time: " << drt + rdrt << " microseconds"
                  << std::endl;
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

    KVPair* kv = nullptr;
    /* a little redundant work seems to benefit small records insert,
     * because metadata cache line reflush is harmful, see also in acquire interface */
    /*if(kWriteOpt)*/ kv = (KVPair*) thread_limbo().acquire(kv_len);

    if(kv != nullptr) { // write kv pair into expired object
      bool same = same_cache_line(kv, kv_len);
      kv->invalidate();  // invalid the expired object, release memory order
      if(!kEnhancedADR && !same) wait_write_back(kv, sizeof(KVPair));
      KVPair::make_kv(kv, key, value, vlen);
      if(!kEnhancedADR && !same) wait_write_back(kv, kv_len);
      kv->set_control(version, false);
      // write back data into medium even on eADR-supported platforms
      if(same) wait_write_back(kv, kv_len);
      else wait_write_back(kv, sizeof(KVPair));
    } else { // write kv pair into newly allocated object
      auto obj = slab_.acquire(kv_len);
      kv = (KVPair*) obj.pointer();
      KVPair::make_kv(kv, key, value, vlen);
      kv->set_control(version, false);
      wait_write_back(kv, kv_len);
      obj.publish(true, 0, true);
    }

    KVPair* old = index_.upsert(kv, code);
    if(old == (KVPair*) internal::kNotFound) return true; // insertion succeeds
    if(old == (KVPair*) internal::kExpired) old = kv;

    if(!kWriteOpt) index_.get_epoch().retire([&, old]() { slab_.release(old); });
    else index_.get_epoch().retire([&, old]() { thread_limbo().release(old); });

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

    KVPair* kv = nullptr;
    /*if(kWriteOpt)*/ kv = (KVPair*) thread_limbo().acquire(kv_len);

    if(kv != nullptr) { // write kv pair into expired object
      bool same = same_cache_line(kv, kv_len);
      kv->invalidate();  // invalid the expired object
      if(!kEnhancedADR && !same) wait_write_back(kv, sizeof(KVPair));
      KVPair::make_kv(kv, key, value, vlen);
      if(!kEnhancedADR && !same) wait_write_back(kv, kv_len);
      kv->set_control(version, false);
      if(same) wait_write_back(kv, kv_len);
      else wait_write_back(kv, sizeof(KVPair));
    } else { // write kv pair into newly allocated object
      auto obj = slab_.acquire(kv_len);
      kv = (KVPair*) obj.pointer();
      KVPair::make_kv(kv, key, value, vlen);
      kv->set_control(version, false);
      wait_write_back(kv, kv_len);
      obj.publish(true, 0, true);
    }

    KVPair* old = index_.update(kv, code);
    if(old == (KVPair*) internal::kNotFound) { // try to update a nonexisting kv
      // if crashed here, leads to an insertion (upsert)
      // such case should be prevented by upper level concurrency control
      if(!kWriteOpt) slab_.release(kv);
      else thread_limbo().release(kv);
      return false;
    }
    // update succeed, but kv is expired
    if(old == (KVPair*) internal::kExpired) old = kv;
    if(!kWriteOpt) index_.get_epoch().retire([&, old]() { slab_.release(old); });
    else index_.get_epoch().retire([&, old]() { thread_limbo().release(old); });

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
    KVPair* kv = nullptr;
    /*if(kWriteOpt)*/ kv = (KVPair*) thread_limbo().acquire(kv_len);
    if(kv != nullptr) {
      bool same = same_cache_line(kv, kv_len);
      kv->invalidate();  // invalid the expired object
      if(!kEnhancedADR && !same) wait_write_back(kv, sizeof(KVPair));
      KVPair::make_kv(kv, key, nullptr, 0);
      if(!kEnhancedADR && !same) wait_write_back(kv, kv_len);
      kv->set_control(version, true);  // tombstone
      if(same) wait_write_back(kv, kv_len);
      else wait_write_back(kv, sizeof(KVPair));
    } else {
      auto obj = slab_.acquire(kv_len);
      kv = (KVPair*) obj.pointer();
      KVPair::make_kv(kv, key, nullptr, 0);
      kv->set_control(version, true); // tombstone
      wait_write_back(kv, kv_len);
      obj.publish(true, 0, true);
    }
    tomb_size_.fetch_add(slab_.region_size(kv));

    KVPair* old = index_.upsert(kv, code);
    if(old == (KVPair*) internal::kNotFound) { // try to delete a non-existing kv
      if(!kWriteOpt) slab_.release(kv);
      else thread_limbo().release(kv);
      return false;
    }
    // note: if another thread's operation is update, it actually does insertion
    // cause logically the kv pair has deleted by current operation
    if(old == (KVPair*) internal::kExpired) old = kv;
    if(!kWriteOpt) index_.get_epoch().retire([&, old]() { slab_.release(old); });
    else index_.get_epoch().retire([&, old]() { thread_limbo().release(old); });

    return true;
  }

  /**
   * @brief number of kv pairs (including tombstone), thread-unsafe
   * */
  size_t size() { return index_.size(); }

  /**
   * @brief used extents size, including free extents
   * */
  size_t used_pm_size() const { return slab_.used_size(); }
};

}

#endif //SLABSTORE_HASH_STORE_H
