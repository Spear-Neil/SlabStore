/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_META_FILE_H
#define SLABSTORE_META_FILE_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>
#include <vector>

#include "const.h"
#include "pptr.h"
#include "desc.h"
#include "persist.h"
#include "fs-util.h"

namespace SlabStore {

/**
 * kChaotic: nvm-pool initialization hasn't been properly finished \n
 * kVolatile: nvm-pool is opened and the allocator is being used by a certain app; if the system crashed
 * in this state, some metadata has to be rebuilt through memory object persistent token after recover \n
 * kConsistent: nvm-pool is closed correctly, all metadata has been flushed to nvm correctly
 * */
enum StatCode : uint64_t { kChaotic = 0, kVolatile, kConsistent };

/**
 * @brief global information for nvm-allocator located at the head of metafile,
 * mainly in charge of extent descriptor and MediumMeta management
 * */
class alignas(kPageSize) MetaHead {
  StatCode stat_;      // nvm-allocator stat
  size_t size_;        // pool size (extent segment size)
  size_t mind_;        // index to the valid meta
  pptr64_t occupied_;  // half-used extents (have free regions, all runs may have been allocated), for fast recovery
  pptr64_t ext_desc_;  // start address of ExtDesc Segment
  size_t ext_total_;   // total extent number (kExtentSize per Extent)
  pptr64_t med_meta_;  // start address of MedMeta Segment
  size_t med_total_;   // total MedMeta number

  struct alignas(kCacheLineSize) DescMeta {
    size_t ext_used;    // used extent number including free extent
    pptr64_t ext_free;  // extent free list
    size_t med_used;    // used MedMeta number including free MedMeta
    pptr64_t med_free;  // MedMeta free list
  } meta_[2];

  static constexpr bool kLogInfo = SlabConst::kLogInfo;
  static constexpr size_t kExtentSize = SlabConst::kExtentSize;
  static constexpr size_t kMedRunBase = SlabConst::kMediumRunBase;
  // for the sake of a simpler MediumMeta allocation policy, allocate kNBaseExtent MediumMeta
  // for all extents of different Run size with a cost of some space wasting
  static constexpr size_t kNBaseExtent = kExtentSize / kMedRunBase;

  typedef std::pair<ExtentDesc*, size_t> extent_id; // <descriptor, index of descriptor/extent>

 private:
  /**
   * @brief allocate a meta array for extent containing medium regions
   * @param mind the index of the meta to be taking effect
   * @return the start address of meta array
   * @note may allocate the meta array from reclaimed med list
   * */
  MediumMeta* allocate_meta(size_t mind) {
    assert(mind < 2 && mind != mind_);
    DescMeta& meta = meta_[mind];
    // allocate from reclaimed/free list
    if(meta.med_free != nullptr) {
      auto runs = (MediumMeta*) meta.med_free.load();
      assert(runs - (MediumMeta*) med_meta_.load() < med_total_);
      // only update head meta and don't update runs.next
      meta.med_free = runs->next();
      return runs;
    }

    auto runs = (MediumMeta*) med_meta_.load() + meta.med_used;
    meta.med_used += kNBaseExtent;
    return runs;
  }

  /**
   * @brief allocate an extent from extent reclaimed/free list for Medium Allocation
   * @param type region type, means how to use an extent
   * @param run_case the index of RunCase
   * @param size run size (small & medium) or region size (large)
   * @return descriptor (initialized) and index of the descriptor/extent
   * */
  extent_id complex_reclaim(RegionType type, size_t run_case, size_t size) {
    assert(type == kMedium && meta_[mind_].ext_free != nullptr);
    size_t oid = mind_, nid = (mind_ + 1) % 2;

    meta_[nid] = meta_[oid];  // copy meta and then update
    MediumMeta* runs = allocate_meta(nid);
    auto desc = (ExtentDesc*) meta_[nid].ext_free.load();
    void* next = desc->next();
    size_t index = desc - (ExtentDesc*) ext_desc_.load();
    assert(index < ext_total_);
    // do not change the next field of the descriptor
    desc->construct(type, run_case, size, next, runs, false);
    meta_[nid].ext_free = next;
    persist_write_back(&meta_[nid], sizeof(DescMeta));
    persist_wait_finish();

    mind_ = nid; // failure atomic
    persist_write_back(&mind_, sizeof(mind_));
    persist_wait_finish();
    return {desc, index};
  }

  /**
   * @brief allocate an extent from extent reclaimed/free list for Small/Large Allocation
   * @param type region type, means how to use an extent
   * @param run_case the index of RunCase
   * @param size run size (small & medium) or region size (large)
   * @return descriptor (initialized) and index of the descriptor/extent
   * */
  extent_id plain_reclaim(RegionType type, size_t run_case, size_t size) {
    assert(type == kSmall || type == kLarge && meta_[mind_].ext_free != nullptr);
    DescMeta& meta = meta_[mind_];
    auto desc = (ExtentDesc*) meta.ext_free.load();
    void* next = desc->next();
    size_t index = desc - (ExtentDesc*) ext_desc_.load();
    assert(index < ext_total_);
    // do not change the next field of the descriptor
    desc->construct(type, run_case, size, next, nullptr, true);

    meta.ext_free = next; // failure atomic
    persist_write_back(&meta, sizeof(DescMeta));
    persist_wait_finish();
    return {desc, index};
  }

  /**
   * @brief allocate an extent from extent reclaimed/free list for Small/Medium/Large Allocation
   * @param type region type, means how to use an extent
   * @param run_case the index of RunCase
   * @param size run size (small & medium) or region size (large)
   * @return descriptor (initialized) and index of the descriptor/extent
   * */
  extent_id reclaim(RegionType type, size_t run_case, size_t size) {
    // no more free extent
    if(meta_[mind_].ext_free == nullptr) { return {nullptr, 0}; }
    if(type == kMedium) {
      // allocate extent for medium allocation request from reclaimed list
      return complex_reclaim(type, run_case, size);
    }
    // allocate extent for small & large allocation request from reclaimed list
    return plain_reclaim(type, run_case, size);
  }

  /**
   * @brief allocate an extent and its meta for medium allocation request
   * @param type region type, means how to use an extent
   * @param size run size (small & medium) or region size (large & huge)
   * @return descriptor (initialized) and index of the descriptor/extent
   * */
  extent_id complex_allocate(RegionType type, size_t run_case, size_t size) {
    assert(type == kMedium && kExtentSize % size == 0);
    assert(meta_[mind_].med_used <= med_total_);
    size_t oid = mind_, nid = (mind_ + 1) % 2;

    meta_[nid] = meta_[oid];  // copy meta and then update
    MediumMeta* runs = allocate_meta(nid);
    size_t index = meta_[nid].ext_used;
    auto desc = (ExtentDesc*) ext_desc_.load() + index;
    desc->construct(type, run_case, size, nullptr, runs, false);
    meta_[nid].ext_used += 1;
    persist_write_back(&meta_[nid], sizeof(DescMeta));
    persist_wait_finish();

    mind_ = nid; // failure atomic
    persist_write_back(&mind_, sizeof(mind_));
    persist_wait_finish();
    return {desc, index};
  }

  /**
   * @brief allocate an extent without run management meta allocation (small & large)
   * @param type region type, means how to use an extent
   * @param size run size (small & medium) or region size (large & huge)
   * @return descriptor (initialized) and index of the descriptor/extent
   * */
  extent_id plain_allocate(RegionType type, size_t run_case, size_t size) {
    assert(type == kSmall || type == kLarge);
    DescMeta& meta = meta_[mind_];
    assert(meta.ext_free == nullptr && meta.ext_used < ext_total_);
    size_t index = meta.ext_used;
    auto desc = (ExtentDesc*) ext_desc_.load() + index;
    desc->construct(type, run_case, size, nullptr, nullptr, true);
    meta.ext_used += 1; // failure atomic
    persist_write_back(&meta, sizeof(DescMeta));
    persist_wait_finish();
    return {desc, index};
  }

  /**
   * @brief allocate an extent from unused extents for small, medium, large allocation
   * @param type region type, means how to use an extent
   * @param size run size (small & medium) or region size (large & huge)
   * @return descriptor (initialized) and index of the descriptor/extent
   * */
  extent_id allocate(RegionType type, size_t run_case, size_t size) {
    if(meta_[mind_].ext_used >= ext_total_) { return {nullptr, 0}; } // no more space
    if(type == kMedium) { // allocate extent for medium allocation request
      return complex_allocate(type, run_case, size);
    }
    // allocate extent for small & large allocation request
    return plain_allocate(type, run_case, size);
  }

  /**
   * @brief release an extent and its meta for medium allocation request
   * @param desc the descriptor to the extent
   * */
  void complex_release(ExtentDesc* desc) {
    assert(desc->type() == kMedium);
    size_t oid = mind_, nid = (mind_ + 1) % 2;

    auto runs = (MediumMeta*) desc->runs();
    meta_[nid] = meta_[oid]; // copy meta and then update
    // first: link free extent and its run management meta to the reclaimed list
    desc->next() = meta_[nid].ext_free;
    runs->next() = meta_[nid].med_free;
    meta_[nid].ext_free = desc, meta_[nid].med_free = runs;
    persist_write_back(desc, sizeof(ExtentDesc));
    persist_write_back(runs, sizeof(MediumMeta));
    persist_write_back(&meta_[nid], sizeof(DescMeta));
    persist_wait_finish();

    mind_ = nid; // failure atomic
    persist_write_back(&mind_, sizeof(mind_));
    persist_wait_finish();
    // second: update the descriptor to invalid (for fast recovery)
    desc->type() = kInvalid;
    persist_write_back(desc, sizeof(ExtentDesc));
    persist_wait_finish();
  }

  /**
   * @brief release an extent without run management meta release (small & large)
   * @param desc the descriptor to the extent
   * */
  void plain_release(ExtentDesc* desc) {
    assert(desc->type() == kLarge || desc->type() == kSmall);
    DescMeta& meta = meta_[mind_];

    // first: link the free extent to the reclaimed list
    desc->next() = meta.ext_free;
    persist_write_back(desc, sizeof(ExtentDesc));
    persist_wait_finish();
    meta.ext_free = desc;  // failure atomic
    persist_write_back(&meta, sizeof(DescMeta));
    persist_wait_finish();

    // second: update the descriptor to invalid (for fast recovery)
    desc->type() = kInvalid;
    persist_write_back(desc, sizeof(ExtentDesc));
    persist_wait_finish();
  }

 public:
  MetaHead() = delete;

  ~MetaHead() = delete;

  MetaHead(const MetaHead&) = delete;

  MetaHead& operator=(const MetaHead&) = delete;

  /**
   * @brief initialize nvm-allocator meta-head
   * @param size pool size (extent segment size)
   * @param ext start address of ExtDesc Segment
   * @param med start address of MedMeta Segment
   * */
  void init(size_t size, void* ext, void* med) {
    stat_ = kChaotic;
    persist_write_back(&stat_, sizeof(StatCode));
    persist_wait_finish();

    size_ = size, mind_ = 0, occupied_ = nullptr;
    ext_desc_ = ext, ext_total_ = size / kExtentSize;
    med_meta_ = med, med_total_ = size / kMedRunBase;
    meta_[0].ext_used = 0, meta_[0].ext_free = nullptr;
    meta_[0].med_used = 0, meta_[0].med_free = nullptr;
    persist_write_back(this, sizeof(MetaHead));
    persist_wait_finish();

    stat_ = kVolatile;
    persist_write_back(&stat_, sizeof(StatCode));
    persist_wait_finish();
  }

  /**
   * @brief reboot and read all half-used extents after normal shutdown/exit
   * @param extents all half-used extents if the pool is correctly closed
   * @return whether the pool is correctly closed (ok for allocation)
   * */
  bool reboot(std::vector<ExtentDesc*>& extents) {
    if(stat_ == kChaotic || (stat_ != kConsistent && stat_ != kVolatile)) {
      fprintf(stderr, "[ERROR]: pool initialization failed\n");
      exit(EXIT_FAILURE);
    }

    if(stat_ == kConsistent) { // the pool is correctly closed
      ExtentDesc* desc = (ExtentDesc*) occupied_.load(), * next;
      for(; desc != nullptr; desc = next) {
        assert(desc->type() != kInvalid);
        extents.push_back(desc);
        next = (ExtentDesc*) desc->next().load();
        desc->next() = nullptr;
        persist_write_back(desc, sizeof(ExtentDesc));
      }

      stat_ = kVolatile, occupied_ = nullptr;
      assert((uintptr_t) &stat_ - (uintptr_t) this <= kCacheLineSize);
      assert((uintptr_t) &occupied_ - (uintptr_t) this <= kCacheLineSize);
      persist_write_back(this, kCacheLineSize);
      persist_wait_finish();

      return true;
    }

    assert(stat_ == kVolatile);
    return false;
  }

  /**
   * @brief persist allocator's state before close
   * */
  void shutdown() {
    // persist allocator global meta information before atomically update StatCode
    persist_write_back(this, sizeof(MetaHead));
    persist_wait_finish();
    stat_ = kConsistent; // failure atomic
    persist_write_back(&stat_, sizeof(StatCode));
    persist_wait_finish();

    if(kLogInfo) {
      printf("[INFO]: total extents count: %zu, used extents (i.e. free extents, full/half-used extents)"
             " count: %zu\n", ext_total_, meta_[mind_].ext_used);
      size_t free_count = 0, half_count = 0, full_count = 0;
      ExtentDesc* desc = (ExtentDesc*) meta_[mind_].ext_free.load();
      for(; desc != nullptr; desc = (ExtentDesc*) desc->next().load()) free_count++;
      desc = (ExtentDesc*) occupied_.load();
      for(; desc != nullptr; desc = (ExtentDesc*) desc->next().load()) half_count++;
      full_count = meta_[mind_].ext_used - free_count - half_count;
      printf("[INFO]: free extents count: %zu, half-used extents count: %zu, full-used extents count: %zu\n",
             free_count, half_count, full_count);
      printf("[INFO]: allocator closed, pool size (virtual space): %f GiB, actual used virtual space: %f GiB \n",
             (double) size_ / kGigaBytes, (double) (half_count + full_count) * kExtentSize / kGigaBytes);
      fflush(stdout);
    }
  }

  /**
   * @brief record a half-used extent before allocator close
   * @param desc the descriptor to the extent
   * */
  void record(ExtentDesc* desc) {
    assert((uintptr_t) desc % kCacheLineSize == 0);
    assert((uintptr_t) desc >= (uintptr_t) (void*) ext_desc_);
    assert((uintptr_t) desc < (uintptr_t) ((ExtentDesc*) ext_desc_.load() + ext_total_));
    desc->next() = occupied_, occupied_ = desc;
    persist_write_back(desc, sizeof(ExtentDesc));
    // no waiting for finish, failure atomicity guaranteed by stat
  }

  /**
   * @brief acquire an extent
   * @param type region type, means how to use an extent
   * @param run_case the index of RunCase
   * @param size run size (small & medium) or region size (large)
   * @return descriptor (initialized) and index of the descriptor/extent
   * */
  extent_id acquire(RegionType type, size_t run_case, size_t size) {
    auto reuse = reclaim(type, run_case, size);
    if(reuse.first != nullptr) return reuse;
    return allocate(type, run_case, size);
  }

  /**
   * @brief release an extent
   * @param desc the descriptor to the extent
   * */
  void release(ExtentDesc* desc) {
    assert((uintptr_t) desc % kCacheLineSize == 0);
    assert((uintptr_t) desc >= (uintptr_t) (void*) ext_desc_);
    assert((uintptr_t) desc < (uintptr_t) ((ExtentDesc*) ext_desc_.load() + ext_total_));
    RegionType type = desc->type();
    if(type == kMedium) { complex_release(desc); }
    else { plain_release(desc); }
  }

  /**
   * @brief pool size (extent segment size)
   * */
  size_t size() const { return size_; }

  /**
   * @brief translate index into pointer to corresponding extent descriptor
   * */
  ExtentDesc* descriptor(size_t index) const {
    assert(index < ext_total_);
    return (ExtentDesc*) ext_desc_.load() + index;
  }

  /**
   * @brief translate extent descriptor to index of extent/descriptor
   * */
  size_t index(ExtentDesc* desc) const {
    assert((uintptr_t) desc % kCacheLineSize == 0);
    assert((uintptr_t) desc >= (uintptr_t) (void*) ext_desc_);
    assert((uintptr_t) desc < (uintptr_t) ((ExtentDesc*) ext_desc_.load() + ext_total_));
    assert(desc - (ExtentDesc*) ext_desc_.load() < ext_total_);
    return desc - (ExtentDesc*) ext_desc_.load();
  }
};

static_assert(sizeof(MetaHead) == kPageSize);


class MetaFile {
  int fd_;             // meta file descriptor
  void* start_;        // start address
  size_t size_;        // meta file size, in bytes (logical)
  std::string path_;   // meta file path

  /*        meta file format
   * ------------------------------ aligned on kPageSize
   *     -- MetaHead Segment --
   *
   * MetaHead for Extent Management
   * ------------------------------ aligned on kPageSize
   *     -- ExtDesc Segment --
   *
   * ExtentDesc w.r.t. Extent
   * ------------------------------ aligned on kPageSize
   *     -- MedMeta Segment --
   *
   * MediumMeta for medium alloc
   * ------------------------------
   */

  static constexpr size_t kMetaHeadSize = sizeof(MetaHead);
  static constexpr size_t kExtDescSize = sizeof(ExtentDesc);
  static constexpr size_t kMedMetaSize = sizeof(MediumMeta);
  static constexpr size_t kExtentSize = SlabConst::kExtentSize;
  static constexpr size_t kMedRunBase = SlabConst::kMediumRunBase;
  static constexpr size_t kMaxAttempts = 2; // max times of attempts of mmap

  static_assert(kMetaHeadSize % kPageSize == 0);

 private:
  /**
   * @brief mmap meta file into memory
   * */
  void mmap_vspace() {
    DEBUG_COND_ERROR(fd_ == -1, "invalid meta file descriptor");
    DEBUG_COND_ERROR(size_ % kPageSize != 0, "invalid meta file size");
    size_t attempt = 0;
    while(true) {
      start_ = fs_file_mmap(fd_, size_, nullptr);
      if(start_ != MAP_FAILED) break;
      if(++attempt >= kMaxAttempts) {
        fprintf(stderr, "[ERROR]: mmap meta file failed after %zu attempts \n", attempt);
        exit(EXIT_FAILURE);
      }
    }
  }

 public:
  MetaFile() : fd_(-1), start_(nullptr), size_(-1), path_() {}

  ~MetaFile() {
    if(fd_ != -1) { // check whether the file is opened
      head().shutdown(); // persist allocator meta information before close
      int res = fs_file_unmap(start_, size_);
      if(res != 0) {
        fprintf(stderr, "[ERROR]: unknown error, failed to unmap meta file\n");
        exit(EXIT_FAILURE);
      }
      res = fs_file_close(fd_);
      if(res != 0) {
        fprintf(stderr, "[ERROR]: unknown error, failed to close meta file\n");
        exit(EXIT_FAILURE);
      }
    }
  }

  MetaFile(const MetaFile&) = delete;

  MetaFile& operator=(const MetaFile&) = delete;

  /**
   * @brief create a meta file
   * @param path meta file path
   * @param size pool size (extent segment size)
   * */
  void create(const std::string& path, size_t size) {
    size_t ext_desc = size / kExtentSize * kExtDescSize;
    size_t med_meta = size / kMedRunBase * kMedMetaSize;
    assert(ext_desc % kPageSize == 0 && med_meta % kPageSize == 0);
    path_ = path, size_ = kMetaHeadSize + ext_desc + med_meta;

    fd_ = fs_file_open(path_.data());
    fs_file_resize(fd_, size_);
    mmap_vspace();
    void* ext_seg = (void*) ((uintptr_t) start_ + kMetaHeadSize);
    void* med_seg = (void*) ((uintptr_t) ext_seg + ext_desc);
    head().init(size, ext_seg, med_seg);
  }

  /**
   * @brief open an existing meta file
   * @param path meta file path
   * */
  void open(const std::string& path) {
    path_ = path;
    bool exist = !fs_path_exist(path_.data());
    if(!exist) {
      fprintf(stderr, "[ERROR]: unknown error, meta file doesn't exist\n");
      exit(EXIT_FAILURE);
    }
    fd_ = fs_file_open(path_.data());
    size_ = fs_file_length(path_.data());
    mmap_vspace();
  }

  /**
   * @brief MetaHead for ExtDesc and MedMeta management
   * */
  MetaHead& head() const { return *(MetaHead*) start_; }

  /**
   * @brief meta file size
   * */
  size_t size() const { return size_; }
};

}

#endif //SLABSTORE_META_FILE_H
