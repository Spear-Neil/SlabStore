/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_HASH_TABLE_H
#define SLABSTORE_HASH_TABLE_H

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <atomic>
#include <cstring>
#include <type_traits>
#include <unordered_set>

#include "util.h"

/**
 * @brief an brief implementation of concurrent extendible hash table
 * @note currently, we do not implement delete/remove operation and
 * hence we also do not care about how to shrink the hash table.
 * */
namespace SlabStore {

namespace internal {

using util::hash;
using util::popcount;
using util::index_least0;
using util::index_least1;
using util::cmpeq_int8_simd128;
using util::cpu_pause;
using util::String;
using util::Epoch;
using util::VerLock;
using util::DensePointer;
using util::AtomicDense;

struct DefaultHashConfig {
  static constexpr bool kInMemStore = true;        // all kv pairs reside in memory
  static constexpr bool kOptUpdate = false;        // cas optimized update
  static constexpr bool kTimeStamp = false;        // timestamp for update, precondition: kOptUpdate is enabled
  static constexpr size_t kNSlotInBucket = 48;     // the number of slots in a hash bucket
  static constexpr size_t kNBucketInSegment = 64;  // the number of buckets in a hash segment
  static constexpr size_t kInitDepth = 4;          // init global depth, 2^4 = 16, 2 cache lines

  static_assert(kNSlotInBucket == 16 || kNSlotInBucket == 32 || kNSlotInBucket == 48);
  static_assert(popcount(kNBucketInSegment) == 1 && kNBucketInSegment <= 256);
};

// Layout of each kv's hash code
// |bucket index (8 bits) |bucket tag (8 bits) |slot tag (16 bits) |segment index (32 bits)|
template<typename K>
inline uint64_t hash_code(const K& key) {
  if constexpr(std::is_same<K, String>()) {
    return hash((void*) key.str, key.len);
  } else { return hash(key); }
}

inline size_t bucket_index(uint64_t code, size_t count) { return (code >> 56) % count; }

inline uint8_t bucket_tag(uint64_t code) { return (code >> 48) & 0x00FFul; }

inline uint16_t slot_tag(uint64_t code) { return (code >> 32) & 0xFFFFul; }

inline size_t segment_index(uint64_t code, size_t depth) { return code & ((0x01ul << depth) - 1); }

enum StatusCode { kNotFound = 0, kNoSpace = 1, kExpired = 2 };

template<typename K, typename V, typename HashConfig>
class alignas(64) HashBucket {
  typedef util::KVPair<K, V> KVPair;
  static constexpr bool kInMemStore = HashConfig::kInMemStore;
  static constexpr bool kOptUpdate = HashConfig::kOptUpdate;
  static constexpr bool kTimeStamp = HashConfig::kTimeStamp;
  static constexpr size_t kNSlot = HashConfig::kNSlotInBucket;
  static constexpr size_t kNBucket = HashConfig::kNBucketInSegment;
  static constexpr std::memory_order load_order = std::memory_order_relaxed;
  static constexpr std::memory_order store_order = std::memory_order_relaxed;

  VerLock lock_;             // operation on this bucket needs to hold this lock
  uint64_t bitmap_: 48;      // whether the corresponding kvs is used
  uint8_t depth_;            // segment/bucket local depth
  uint8_t index_;            // bucket index in the corresponding segment
  uint8_t tags_[kNSlot];     // 8-bit bucket tag of the corresponding kvs.key
  AtomicDense kvs_[kNSlot];  // pointer kv with its 16-bit slot tag

 private:
  uint64_t compare_equal(void* p, char c) const {
    if constexpr(kNSlot == 16) {
      return cmpeq_int8_simd128(p, c);
    } else if constexpr(kNSlot == 32) {
      uint64_t m0 = cmpeq_int8_simd128(p, c);
      uint64_t m1 = cmpeq_int8_simd128((char*) p + 16, c);
      return (m1 << 16) | m0;
    } else if constexpr(kNSlot == 48) {
      uint64_t m0 = cmpeq_int8_simd128(p, c);
      uint64_t m1 = cmpeq_int8_simd128((char*) p + 16, c);
      uint64_t m2 = cmpeq_int8_simd128((char*) p + 32, c);
      return (m2 << 32) | (m1 << 16) | m0;
    }
    assert(false);
  }

 public:
  /**
   * @brief default constructor, create an empty bucket
   * */
  HashBucket() : lock_(), bitmap_(0), depth_(0), index_(0), tags_{}, kvs_{} {}

  ~HashBucket() {
    if constexpr(kInMemStore) {
      uint64_t mask = bitmap_;
      while(mask) {
        size_t idx = index_least1(mask);
        DensePointer kv = kvs_[idx].load(load_order);
        ((KVPair*) kv.pointer())->~KVPair();
        free(kv.pointer());
        mask &= ~(0x01ul << idx);
      }
    }
  }

  /**
   * @brief split the left bucket, move corresponding kvs into this bucket
   * */
  HashBucket(HashBucket&& left) noexcept  // only used for split
    : lock_(), bitmap_(0), depth_(left.depth_), index_(left.index_), tags_{}, kvs_{} {
    assert(left.lock().locked());
    memcpy(tags_, left.tags_, sizeof(tags_));
    memcpy(kvs_, left.kvs_, sizeof(kvs_));

    uint64_t move_mask = 0x01ul << left.depth_;
    uint64_t mask = left.bitmap_, candidates = 0;
    while(mask) { // locate all kvs to be moved to this bucket
      size_t idx = index_least1(mask);
      void* kv = left.kvs_[idx].load(load_order).pointer();
      uint64_t code = hash_code(((KVPair*) kv)->key);
      if(code & move_mask) {
        candidates |= 0x01ul << idx;
        if constexpr(kOptUpdate) { // get the latest kv order, because other threads may update this kv simultaneously
          auto latest = left.kvs_[idx].exchange(DensePointer());
          if(kvs_[idx].load(load_order) != latest) {
            kvs_[idx].store(latest, store_order);
          }
        } // leads to a little overhead
      }
      mask &= ~(0x01ul << idx);
    }

    bitmap_ = candidates, depth_++;
    left.bitmap_ &= ~candidates, left.depth_++;
  }

  HashBucket& operator=(const HashBucket&) = delete;

  VerLock& lock() { return lock_; }

  uint8_t& depth() { return depth_; }

  uint8_t& index() { return index_; }

  /**
   * @brief number of kv pairs in current bucket, thread unsafe
   * */
  size_t size() const { return popcount(bitmap_); }

  /**
   * @brief insert a key-value pair into current bucket or
   * update an existing key-value pair in current bucket
   * @param kv the key-value pair trying to insert or update
   * @param code the corresponding hash code of kv.key
   * */
  KVPair* upsert(KVPair* kv, uint64_t code) {
    assert(lock().locked() && kv != nullptr);
    assert(hash_code(kv->key) == code);
    assert(bucket_index(code, kNBucket) == index_);

    uint64_t mask = bitmap_ & compare_equal(tags_, bucket_tag(code));
    while(mask) {  // check whether the key exists or not
      size_t idx = index_least1(mask);
      DensePointer old = kvs_[idx].load(load_order);
      if(old.remain() == slot_tag(code) &&
         ((KVPair*) old.pointer())->key == kv->key) {
        if constexpr(kOptUpdate) {
          if constexpr(kTimeStamp) { // for pmem hash store
            while(true) {
              if(((KVPair*) old.pointer())->load_version() >= kv->load_version()) {
                return (KVPair*) kExpired; // the kv is expired
              }
              if(kvs_[idx].compare_exchange_strong(old, DensePointer(kv, slot_tag(code)))) {
                break;
              }

              cpu_pause(); // pause and then reload the latest
              old = kvs_[idx].load(load_order);
            }
          } else { // get the latest kv, and update
            old = kvs_[idx].exchange(DensePointer(kv, slot_tag(code)));
          }
        } else {
          kvs_[idx].store(DensePointer(kv, slot_tag(code)), store_order);
        }
        return (KVPair*) old.pointer();
      } // update existing kv succeed
      mask &= ~(0x01ul << idx);
    }

    // a new key-value pair, find an empty slot
    size_t idx = index_least0(bitmap_);
    // the current bucket is full, need segment split
    if(idx == kNSlot) return (KVPair*) kNoSpace;

    // not found an existing kv, insert new kv
    kvs_[idx].store(DensePointer(kv, slot_tag(code)), store_order);
    tags_[idx] = bucket_tag(code), bitmap_ |= (0x01ul << idx);
    assert((void*) kNotFound == nullptr);
    return (KVPair*) kNotFound; // insertion succeeds
  }

  /**
   * @brief cas optimized update operation, without holding a lock on the bucket
   * @note only valid when kOptUpdate == true
   * */
  template<bool Enable = kOptUpdate, std::enable_if_t<Enable, int> = 0>
  KVPair* update(KVPair* kv, uint64_t code) {
    assert(kv != nullptr && hash_code(kv->key) == code);
    assert(bucket_index(code, kNBucket) == index_);
    uint64_t mask = bitmap_ & compare_equal(tags_, bucket_tag(code));
    while(mask) {
      int idx = index_least1(mask);
      DensePointer old = kvs_[idx].load(load_order);
      while(old != DensePointer() && old.remain() == slot_tag(code)
            && ((KVPair*) old.pointer())->key == kv->key) {
        if constexpr(kTimeStamp) { // for pmem hash store
          if(((KVPair*) old.pointer())->load_version() >= kv->load_version()) {
            return (KVPair*) kExpired; // the kv is expired
          }
        }
        if(kvs_[idx].compare_exchange_strong(old, DensePointer(kv, slot_tag(code)))) {
          return (KVPair*) old.pointer(); // update succeed
        }

        cpu_pause();// pause and then reload the latest
        old = kvs_[idx].load(load_order);
      }
      mask &= ~(0x01ul << idx);
    }

    // failed because another thread has moved this kv to another bucket
    // failed because the corresponding kv doesn't exist
    return (KVPair*) kNotFound;
  }

  /**
   * @brief disable update operation if kOptUpdate is false
   * @note maybe I should use static label forwarding, SFINAE is interesting but a little antihuman
   * */
  template<bool Enable = kOptUpdate, std::enable_if_t<!Enable, int> = 0>
  KVPair* update(KVPair* kv, uint64_t code) = delete;

  /**
   * @brief lookup a key-value pair in current bucket
   * @param kv the key-value pair trying to lookup
   * @param code the corresponding hash code of key
   * */
  KVPair* lookup(const K& key, uint64_t code) const {
    assert(hash_code(key) == code);
    assert(bucket_index(code, kNBucket) == index_);
    uint8_t* tags = const_cast<uint8_t*>(tags_);
    uint64_t mask = bitmap_ & compare_equal(tags, bucket_tag(code));
    while(mask) { // check whether the key exists or not
      size_t idx = index_least1(mask);
      DensePointer kv = kvs_[idx].load(load_order);
      if(kv != DensePointer() && kv.remain() == slot_tag(code)
         && ((KVPair*) kv.pointer())->key == key) {
        return (KVPair*) kv.pointer();
      } // lookup corresponding kv succeed
      mask &= ~(0x01ul << idx);
    }

    assert((void*) kNotFound == nullptr);
    return (KVPair*) kNotFound; // failed
  }
};

template<typename K, typename V, typename HashConfig>
class alignas(64) HashSegment {
  typedef HashBucket<K, V, HashConfig> Bucket;
  static constexpr size_t kNBucketInSegment = HashConfig::kNBucketInSegment;

  Bucket buckets_[kNBucketInSegment];

 public:
  /**
   * @brief create a new hash segment
   * @param depth local depth
   * */
  explicit HashSegment(size_t depth) : buckets_{} {
    for(size_t ind = 0; ind < kNBucketInSegment; ind++) {
      buckets_[ind].index() = ind, buckets_[ind].depth() = depth;
    }
  }

  ~HashSegment() = default;

  /**
   * @brief split left segment and move corresponding kvs into this segment
   * */
  HashSegment(HashSegment&& left) noexcept: buckets_{} { // only used for split
    for(size_t ind = 0; ind < kNBucketInSegment; ind++) {
      new(buckets_ + ind) Bucket(std::move(left.buckets_[ind]));
    }
    assert(left.depth() == depth());
  }

  HashSegment& operator=(const HashSegment&) = delete;

  /**
   * @brief lock all buckets in current segment
   * */
  void lock_exclusive() { for(auto& bucket : buckets_) bucket.lock().lock_exclusive(); }

  /**
   * @brief unlock all buckets in current segment
   * */
  void unlock_exclusive() { for(auto& bucket : buckets_) bucket.lock().unlock_exclusive(); }

  /**
   * @brief get corresponding bucket
   * @param code hashcode of the corresponding key
   * */
  Bucket* bucket(size_t code) { return &buckets_[bucket_index(code, kNBucketInSegment)]; }

  /**
   * @brief local depth of this segment, thread unsafe
   * */
  size_t depth() const { return const_cast<Bucket&>(buckets_[0]).depth(); }

  /**
   * @brief split current segment into two segment
   * */
  HashSegment* split() { return new HashSegment(std::move(*this)); }

  /**
   * @brief number of kv pairs in current segment, thread unsafe
   * */
  size_t size() const {
    size_t count = 0;
    for(const Bucket& bucket : buckets_)
      count += bucket.size();
    return count;
  }
};

template<typename K, typename V, typename HashConfig = DefaultHashConfig>
class alignas(32) HashTable {
  typedef HashBucket<K, V, HashConfig> Bucket;
  typedef HashSegment<K, V, HashConfig> Segment;
  static constexpr size_t kInitDepth = HashConfig::kInitDepth;
  static constexpr size_t kNSlotInBucket = HashConfig::kNSlotInBucket;
  static constexpr size_t kNBucketInSegment = HashConfig::kNBucketInSegment;

  VerLock lock_;     // segment split and directory doubling need to hold this lock
  Segment** dir_;    // directory (segment entry array)
  size_t depth_;     // global depth (total segment entry count)
  Epoch* epoch_;     // each operation should be guarded through epoch

  template<typename Exclusion>
  struct ExclusiveGuard {
    ExclusiveGuard(Exclusion& ex) : ex_(ex) { ex_.lock_exclusive(); }

    ~ExclusiveGuard() { ex_.unlock_exclusive(); }

    ExclusiveGuard(const ExclusiveGuard&) = delete;

    ExclusiveGuard& operator=(const ExclusiveGuard&) = delete;

   private:
    Exclusion& ex_;
  };

 public:
  typedef util::KVPair<K, V> KVPair;

  /**
   * @brief create a new hash table
   * @param global init global depth (segment entry count / directory size = 2 ^ global)
   * @param local init local depth (segment count = 2 ^ local)
   * */
  HashTable(size_t global = kInitDepth, size_t local = 0) :
    lock_(), dir_(nullptr), depth_(global), epoch_(nullptr) {
    assert(local <= global && global <= 32);
    size_t entry_count = 0x01ul << global;
    size_t segment_count = 0x01ul << local;
    dir_ = new Segment* [entry_count], epoch_ = new Epoch();
    for(size_t ind = 0; ind < entry_count; ind++) {
      if(ind < segment_count) dir_[ind] = new Segment(local);
      else dir_[ind] = dir_[ind % segment_count];
    }
  }

  ~HashTable() {
    size_t global = depth_, local;
    size_t count = 0x01ul << global;
    for(size_t idx = 0; idx < count; idx++) {
      if(dir_[idx] == nullptr) continue;
      local = dir_[idx]->depth();
      size_t npart = count >> local;
      for(size_t pid = 1; pid < npart; pid++) {
        dir_[(pid << local) + idx] = nullptr;
      }
      delete dir_[idx];
    }
    delete[] dir_;
    delete epoch_;
  }

  HashTable(const HashTable&) = delete;

  HashTable& operator=(const HashTable&) = delete;

  /**
   * @brief get epoch-based memory reclaimer, each operation below
   * should be protected by an epoch
   * */
  Epoch& get_epoch() { return *epoch_; }

  /**
   * @brief insert a key-value pair or update an existing key-value pair
   * @param kv the key-value pair to be inserted or updated
   * @param code corresponding hash code of the kv.key
   * @return the old key-value pair
   * */
  KVPair* upsert(KVPair* kv, uint64_t code) {
    assert(kv != nullptr && code == hash_code(kv->key));
    Segment** dir = nullptr;
    size_t global = 0, local = 0;

    while(true) {
      uint64_t version = lock_.lock_shared();
      dir = dir_, global = depth_;
      // directory may be doubling during reading directory and depth, but dir_ and depth_ are not
      // changed in one atomic operation; hereby atomicity is guaranteed through version validation
      if(lock_.version_changed(version)) continue;

      size_t index = segment_index(code, global);

      { // insert a new key-value pair or update an existing key-value pair
        Bucket* bucket = dir[index]->bucket(code);
        // lock the corresponding bucket
        ExclusiveGuard guard(bucket->lock());
        // make sure we get the correct bucket (because some other threads may have moved some
        // kv pairs to its sibling bucket before we really holds the bucket's exclusive lock)
        if(!lock_.unlock_shared(version)) continue;
        KVPair* res = bucket->upsert(kv, code);
        if(res != (KVPair*) kNoSpace) return res;
        local = bucket->depth(); // local depth
        assert(local <= global);
      } // do not move this brace

      { // bucket is full, split the corresponding segment, doubling the directory if necessary
        ExclusiveGuard segment_guard(*dir[index]);
        // some other threads have split this segment, try again
        if(dir[index]->depth() != local) continue;
        Segment* right = dir[index]->split();
        assert(right->depth() == local + 1);

        ExclusiveGuard directory_guard(lock_);
        dir = dir_, global = depth_; // another thread may have doubled directory

        if(local == global) { // directory doubling
          size_t entry_count = 0x01ul << global;
          dir_ = new Segment* [entry_count * 2], depth_ += 1;
          memcpy(dir_, dir, entry_count * sizeof(Segment*));
          memcpy(dir_ + entry_count, dir, entry_count * sizeof(Segment*));
          epoch_->retire([dir]() { delete[] dir; });
          dir = dir_, global = depth_;
        }

        // insert the right segment to the global directory
        assert(local < global);
        size_t part_size = 0x01ul << (local + 1);
        size_t part_count = (0x01ul << global) / part_size;
        // segment index where the size of directory is 2 ^ (local + 1)
        size_t offset = segment_index(code, local) + (0x01ul << local);
        for(size_t pid = 0; pid < part_count; pid++) {
          dir[pid * part_size + offset] = right;
        }
      } // split succeed, try insert again
    }
    assert(false); // not reach
  }

  KVPair* upsert(KVPair* kv) { return upsert(kv, hash_code(kv->key)); }

  /**
   * @brief update an existing kv using cas primitive without lock the corresponding bucket
   * @param kv the key-value pair to be updated
   * @param code corresponding hash code of the kv.key
   * @note This function is valid only when kOptUpdate is enabled
   * */
  KVPair* update(KVPair* kv, uint64_t code) {
    assert(kv != nullptr && code == hash_code(kv->key));
    Segment** dir = nullptr;
    size_t global = 0, local = 0;

    while(true) {
      uint64_t version_dir = lock_.lock_shared();
      dir = dir_, global = depth_;
      if(lock_.version_changed(version_dir)) continue;

      size_t index = segment_index(code, global);
      Bucket* bucket = dir[index]->bucket(code);
      uint64_t version = bucket->lock().lock_shared();
      if(!lock_.unlock_shared(version_dir)) continue;
      KVPair* old = bucket->update(kv, code);
      if(old != (KVPair*) kNotFound) return old; // update succeed
      if(bucket->lock().unlock_shared(version)) break;
    }
    return (KVPair*) kNotFound; // failed
  }

  KVPair* update(KVPair* kv) { return update(kv, hash_code(kv->key)); }

  /**
   * @brief lookup an existing key-value pair with given key
   * @param key the corresponding key
   * @param code corresponding hash code of key
   * @return key-value pair to be required
   * */
  KVPair* lookup(const K& key, uint64_t code) {
    assert(code == hash_code(key));
    Segment** dir = nullptr;
    size_t global = 0, local = 0;

    while(true) {
      uint64_t version_dir = lock_.lock_shared();
      dir = dir_, global = depth_;
      if(lock_.version_changed(version_dir)) continue;

      size_t index = segment_index(code, global);
      Bucket* bucket = dir[index]->bucket(code);
      uint64_t version = bucket->lock().lock_shared();
      if(!lock_.unlock_shared(version_dir)) continue;
      KVPair* kv = bucket->lookup(key, code);
      if(kv != (KVPair*) kNotFound) return kv; // find it
      if(bucket->lock().unlock_shared(version)) break;
    }
    return (KVPair*) kNotFound;
  }

  KVPair* lookup(const K& key) { return lookup(key, hash_code(key)); }

  /**
   * @brief directory size (segment entry count), thread-unsafe
   * */
  size_t directory_size() const { return 0x01ul << depth_; }

  /**
   * @brief total segment count, thread-unsafe
   * */
  size_t segment_count() const {
    std::unordered_set<Segment*> segments;
    segments.reserve(directory_size());
    for(size_t i = 0; i < directory_size(); i++)
      segments.insert(dir_[i]);
    return segments.size();
  }

  /**
   * @brief bucket count, thread-unsafe
   * */
  size_t bucket_count() const { return segment_count() * kNBucketInSegment; }

  /**
   * @brief the number of kv pairs if load factor = 1, thread-unsafe
   * */
  size_t capacity() const { return bucket_count() * kNSlotInBucket; }

  /**
   * @brief the number of kv pairs, thread-unsafe
   * */
  size_t size() const {
    size_t count = 0;
    std::unordered_set<Segment*> segments;
    segments.reserve(directory_size());
    for(size_t i = 0; i < directory_size(); i++) {
      if(segments.find(dir_[i]) == segments.end()) {
        segments.insert(dir_[i]), count += dir_[i]->size();
      }
    }
    return count;
  }
};

}

using internal::DefaultHashConfig;
using internal::HashTable;

}

#endif //SLABSTORE_HASH_TABLE_H
