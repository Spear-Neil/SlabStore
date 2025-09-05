/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_EXTENT_H
#define SLABSTORE_EXTENT_H

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <string>
#include <tuple>
#include <vector>
#include <atomic>
#include <tbb/concurrent_unordered_set.h>

#include "const.h"
#include "desc.h"
#include "fs-util.h"
#include "meta-file.h"
#include "ext-file.h"
#include "util.h"

namespace SlabStore {

using util::rounddown;
using util::MutexLock;
using util::LockGuard;

/**
 * @brief An ExtentCase contains all the extents of a persistent memory pool, responsible
 * for extent allocation. An ExtentBin consists of one meta file and one extent file.
 * */
class ExtentCase {
  MutexLock<> lock_;   // extent (de-)allocation needs to hold the lock
  std::string path_;   // pool directory
  size_t size_;        // pool size (extent segment size)
  MetaFile meta_;      // meta file
  ExtentFile extent_;  // extent file

  tbb::concurrent_unordered_set<ExtentDesc*> recs_; // extents allocated during recovering

  static constexpr bool kLogInfo = SlabConst::kLogInfo;
  static constexpr size_t kDefaultSize = -1;  // use capacity size of nvm device as pool size
  static constexpr size_t kExtentSize = SlabConst::kExtentSize;
  static constexpr size_t kPoolSizeAlign = SlabConst::kPoolSizeAlign;
  inline static const std::string kMetaName = SlabConst::kMetaFileName;
  inline static const std::string kExtentName = SlabConst::kExtentFileName;

 private:
  /**
   * @brief open an existing persistent memory pool
   * */
  void open_impl() {
    extent_.open(path_ + '/' + kExtentName);
    size_ = extent_.pool_size();
    if(size_ % kExtentSize != 0 || size_ % kPoolSizeAlign != 0) {
      fprintf(stderr, "[ERROR]: unknown error, pool size is not aligned\n");
      exit(EXIT_FAILURE);
    }
    meta_.open(path_ + '/' + kMetaName);
    if(size_ != meta_.head().size()) {
      fprintf(stderr, "[ERROR]: unknown error, pool size and meta.head.size do not match\n");
      exit(EXIT_FAILURE);
    }
  }

  /**
   * @brief create a new persistent memory pool
   * */
  void create_impl() {
    fs_dir_make(path_.data());
    if(size_ == kDefaultSize) {
      size_ = fs_device_capacity(path_.data());
    }
    size_ = rounddown(size_, kPoolSizeAlign); // aligned on kPoolSizeAlign
    if(size_ < kPoolSizeAlign) size_ = kPoolSizeAlign; // at least kPoolSizeAlign
    assert(size_ % kExtentSize == 0);

    extent_.create(path_ + '/' + kExtentName, size_);
    meta_.create(path_ + '/' + kMetaName, size_);
  }

 public:
  class RecoverContainer {
    ExtentCase* ext_case_;
    std::atomic<size_t> index_; // currently processing extent index

   public:
    explicit RecoverContainer(ExtentCase* ext_case) : ext_case_(ext_case), index_(0) {}

    ~RecoverContainer() = default;

    RecoverContainer(const RecoverContainer&) = delete;

    RecoverContainer& operator=(const RecoverContainer&) = delete;

    /**
     * @brief get next extent descriptor for recover
     * */
    ExtentDesc* next() {
      size_t count = ext_case_->meta_.head().count();
      while(true) {
        size_t ind = index_.fetch_add(1);
        if(ind >= count) return nullptr; // all extents have been handled
        ExtentDesc* desc = ext_case_->meta_.head().descriptor(ind);
        assert(desc->type() < kTypeCount);
        // skip reclaimed/free extents
        if(desc->type() == kInvalid) continue;
        // skip extents allocated during recovering
        if(!ext_case_->recover_extent(desc)) return desc;
      }
    }
  };

 public:
  ExtentCase() : lock_(), path_(), size_(-1), meta_(), extent_() {}

  ~ExtentCase() {
    if(kLogInfo) { printf("[CLOSE]: %s\n", path_.data()); }
  }

  ExtentCase(const ExtentCase&) = delete;

  ExtentCase& operator=(const ExtentCase&) = delete;

  /**
   * @brief some post-recovery finishing tasks
   * */
  void finish_recover() { recs_.clear(); }

  /**
   * @brief open or create a persistent memory pool
   * @param path persistent memory pool path
   * @param size persistent memory pool size (device capacity by default)
   * @return whether the memory pool exist
   * */
  bool open(const std::string& path, size_t size = kDefaultSize) {
    path_ = path, size_ = size;
    bool exist = !fs_path_exist(path_.data());
    if(exist) { open_impl(); }
    else { create_impl(); }
    return exist;
  }

  /**
   * @brief reboot and read all half-used extents after normal shutdown/exit
   * @param extents all half-used extents if the pool is correctly closed
   * @return whether the pool is correctly closed (ok for allocation)
   * */
  bool reboot(std::vector<ExtentDesc*>& extents) {
    return meta_.head().reboot(extents);
  }

  /**
   * @brief rollback/execute outstanding extent allocation/release transactions before recover
   * @return a RecoverContainer for concurrently scanning the pool
   * */
  RecoverContainer resume() {
    meta_.head().resume();
    return RecoverContainer(this);
  }

  /**
   * @brief preallocate physical space for a specific range of extent file
   * @param ptr the start address of the specific range (PAGE ALIGNED)
   * @param len the length of the specific range (PAGE ALIGNED)
   * */
  void physical_space_alloc(void* ptr, size_t len) {
    extent_.physical_space_alloc(ptr, len);
  }

  /**
   * @brief deallocate physical space for a specific range of extent file
   * @param ptr the start address of the specific range (PAGE ALIGNED)
   * @param len the length of the specific range (PAGE ALIGNED)
   * */
  void physical_space_reclaim(void* ptr, size_t len) {
    extent_.physical_space_reclaim(ptr, len);
  }

  /**
   * @brief get the extent descriptor corresponding to objects/regions
   * */
  ExtentDesc* descriptor(void* ptr) const {
    size_t ind = extent_.locate(ptr);
    return meta_.head().descriptor(ind);
  }

  /**
   * @brief get the extent's start address corresponding to the descriptor
   * */
  void* extent(ExtentDesc* desc) const {
    size_t ind = meta_.head().index(desc);
    return extent_.locate(ind);
  }

  /**
   * @brief check whether the extent is allocated during recovering
   * */
  bool recover_extent(ExtentDesc* desc) {
    return recs_.find(desc) != recs_.end();
  }

  /**
   * @brief acquire an extent from ExtentCase
   * @param type region type, means how to use an extent
   * @param run_case the index of RunCase
   * @param size run size (small & medium) or region size (large)
   * @param recover do extent allocation during recovering
   * @return descriptor (initialized) and start address of the extent
   * */
  extent_t acquire(RegionType type, size_t run_case, size_t size, bool recover) {
    LockGuard guard(lock_);
    MetaHead& head = meta_.head();
    auto [desc, index] = head.acquire(type, run_case, size);
    void* ext = nullptr; // if no more space
    if(desc) ext = extent_.locate(index);
    if(recover) recs_.insert(desc); // record extents allocated during recovering
    if(type == kSmall && desc) physical_space_alloc(ext, kExtentSize); // physical page pre-allocation
    return {desc, ext};
  }

  /**
   * @brief release an extent back to ExtentCase
   * @param desc the corresponding descriptor to the extent
   * @param ext the start address of an extent
   * */
  void release(ExtentDesc* desc, void* ext) {
    assert(size_t(ext) % kExtentSize == 0);
    assert(descriptor(ext) == desc);

    // reclaim physical space first for kSmall extents
    if(desc->type() == kSmall)
      physical_space_reclaim(ext, kExtentSize);
    LockGuard guard(lock_);
    meta_.head().release(desc);
  }

  /**
   * @brief record a half-used extent before close
   * @param desc the corresponding descriptor to the extent
   * */
  void record(ExtentDesc* desc) { meta_.head().record(desc); }

  /**
   * @brief persistent root
   * */
  PersistRoot& root() { return extent_.root(); }
};

}

#endif //SLABSTORE_EXTENT_H
