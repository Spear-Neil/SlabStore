/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_EXT_FILE_H
#define SLABSTORE_EXT_FILE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <string>

#include "const.h"
#include "pptr.h"
#include "persist.h"
#include "fs-util.h"
#include "util.h"

namespace SlabStore {

using util::roundup;

class alignas(kPageSize) PersistRoot {
  /* reserved for application as the root/entrance of a user defined data
   * structure, initialized with nullptr, means this root hasn't been used */
  pptr64_t root_[SlabConst::kRootCount];

  friend class ExtentFile;

 private:
  void init() {
    for(auto& ptr : root_) {
      ptr.store(nullptr);
    }
    persist_concur_flush(this, sizeof(PersistRoot));
    persist_wait_finish();
  }

 public:
  PersistRoot() = delete;

  ~PersistRoot() = delete;

  PersistRoot(const PersistRoot&) = delete;

  PersistRoot& operator=(const PersistRoot&) = delete;

  pptr64_t& operator[](size_t idx) {
    DEBUG_COND_ERROR(idx >= SlabConst::kRootCount, "invalid root index");
    return root_[idx];
  }
};


class ExtentFile {
  int fd_;              // extent file descriptor
  void* start_;         // start address of extent-file
  size_t size_;         // extent file size, in bytes (logical)
  std::string path_;    // extent file path

  /*      extent file format
   * ------------------------------  aligned on kPageSize
   *       -- Root Segment --
   *
   * persistent root, as entrance for link structure
   * ------------------------------  aligned on kExtentSize
   *      -- Extent Segment --
   *      span of 4MB extents
   * ------------------------------
   */

  static constexpr bool kLogInfo = SlabConst::kLogInfo;
  static constexpr size_t kExtentSize = SlabConst::kExtentSize;
  static constexpr size_t kRootSegSize = sizeof(PersistRoot);
  static constexpr size_t kMaxAttempts = 2; // max times of attempts of mmap

  static_assert(kRootSegSize % kPageSize == 0 && kRootSegSize < kExtentSize);

 private:
  /**
   * @brief map extent file into memory
   * */
  void mmap_vspace() {
    DEBUG_COND_ERROR(fd_ == -1, "invalid extent file descriptor");
    void* hint, * addr;
    size_t attempt = 0;
    while(true) {
      hint = fs_mmap_hint(fd_, size_ + kExtentSize);
      hint = (void*) roundup((size_t)
                               hint + kRootSegSize, kExtentSize);
      hint = (void*) ((size_t)
                        hint - kRootSegSize);
      addr = fs_file_mmap(fd_, size_, hint);
      if(addr == hint) break;
      if(++attempt >= kMaxAttempts) {
        fprintf(stderr, "[ERROR]: mmap extent file failed after %zu attempts \n", attempt);
        exit(EXIT_FAILURE);
      }
    }
    start_ = addr;
  }

  /**
   * @brief start address of extent segment
   * */
  void* extents() const { return (void*) ((uintptr_t) start_ + kRootSegSize); }

  /**
   * @brief the end of extent segment/extent file, [start, end)
   * */
  void* end() const { return (void*) ((uintptr_t) start_ + size_); }

 public:
  ExtentFile() : fd_(-1), start_(nullptr), size_(0), path_() {}

  ~ExtentFile() {
    if(fd_ != -1) { // check whether the file is opened
      int res = fs_file_unmap(start_, size_);
      if(res != 0) {
        fprintf(stderr, "[ERROR]: unknown error, failed to unmap extent file\n");
        exit(EXIT_FAILURE);
      }
      res = fs_file_close(fd_);
      if(res != 0) {
        fprintf(stderr, "[ERROR]: unknown error, failed to close extent file\n");
        exit(EXIT_FAILURE);
      }
      if(kLogInfo) {
        double physical_size = (double) fs_space_usage(path_.data()) / kGigaBytes;
        printf("[INFO]: allocator actual used physical space (allocated blocks) size: %f GiB\n", physical_size);
        fflush(stdout);
      }
    }
  }

  ExtentFile(const ExtentFile&) = delete;

  ExtentFile& operator=(const ExtentFile&) = delete;

  /**
   * @brief create an extent file and map into vspace
   * @param path extent file path
   * @param size size of extent segment
   * @note size must be multiple of kMaxExtSize
   * */
  void create(const std::string& path, size_t size) {
    assert(size > 0 && size % kExtentSize == 0);
    path_ = path, size_ = size + kRootSegSize;

    fd_ = fs_file_open(path_.data());
    fs_file_resize(fd_, size_);
    mmap_vspace();
    fs_space_alloc(fd_, 0, kRootSegSize);
    root().init();
  }

  /**
   * @brief open a extent file and mmap to vspace
   * @param path extent file path
   * */
  void open(const std::string& path) {
    path_ = path;
    bool exist = !fs_path_exist(path_.data());
    if(!exist) {
      fprintf(stderr, "[ERROR]: unknown error, extent file doesn't exist\n");
      exit(EXIT_FAILURE);
    }
    fd_ = fs_file_open(path_.data());
    size_ = fs_file_length(path_.data());
    mmap_vspace();
  }

  /**
   * @brief persistent root
   * */
  PersistRoot& root() { return *(PersistRoot*) start_; }

  /**
   * @brief extent file size
   * */
  size_t size() const { return size_; }

  /**
   * @brief extent segment size (pool size)
   * */
  size_t pool_size() const { return size_ - kRootSegSize; }

  /**
   * @brief translate index of an extent into its start address
   * @param index the index of an extent
   * @return the start address of the extent
   * */
  void* locate(size_t index) const {
    assert(index < (size_ - kRootSegSize) / kExtentSize);
    return (void*) ((uintptr_t) extents() + index * kExtentSize);
  }

  /**
   * @brief translate objects/regions in an extent into index of the extent
   * @param ptr any pointers to extents/regions
   * @return the index of corresponding extent/extent descriptor
   * */
  size_t locate(void* ptr) const {
    assert((uintptr_t) ptr >= (uintptr_t) extents());
    assert((uintptr_t) ptr < (uintptr_t) end());
    return ((uintptr_t) ptr - (uintptr_t) extents()) / kExtentSize;
  }

  /**
   * @brief preallocate physical space for a specific range of extent file
   * @param ptr the start address of the specific range (PAGE ALIGNED)
   * @param len the length of the specific range (PAGE ALIGNED)
   * */
  void physical_space_alloc(void* ptr, size_t len) {
    assert((uintptr_t) ptr % kPageSize == 0 && len % kPageSize == 0);
    assert((uintptr_t) ptr >= (uintptr_t) extents());
    assert((uintptr_t) ptr + len <= (uintptr_t) end());
    fs_space_alloc(fd_, (uintptr_t) ptr - (uintptr_t) start_, len);
  }

  /**
   * @brief deallocate physical space for a specific range of extent file
   * @param ptr the start address of the specific range (PAGE ALIGNED)
   * @param len the length of the specific range (PAGE ALIGNED)
   * */
  void physical_space_reclaim(void* ptr, size_t len) {
    assert((uintptr_t) ptr % kPageSize == 0 && len % kPageSize == 0);
    assert((uintptr_t) ptr >= (uintptr_t) extents());
    assert((uintptr_t) ptr + len <= (uintptr_t) end());
    fs_space_reclaim(fd_, (uintptr_t) ptr - (uintptr_t) start_, len);
  }
};

}

#endif //SLABSTORE_EXT_FILE_H
