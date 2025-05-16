/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_SLAB_H
#define SLABSTORE_SLAB_H

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <set>

#include "const.h"
#include "extent.h"
#include "size.h"
#include "run.h"
#include "arena.h"
#include "tcache.h"
#include "util.h"

namespace SlabStore {

using util::ncpus_online;
using util::MutexLock;
using util::LockGuard;

class Allocator {
  ExtentCase* ext_case_; // extent manager for extent (de-)allocation, physical page (de-)allocation, etc.
  SizeClass* sc_;        // mutual conversion between cache bin index and slab size
  size_t nruns_;         // the number of RunCase
  RunCase* run_cases_;   // run (de-)allocation & large region (de-)allocation
  size_t narenas_;       // the number of arena
  Arena* arenas_;        // small & medium region allocation

  static constexpr size_t kCorePerArena = SlabConst::kCorePerArena;
  static constexpr size_t kCorePerRunCase = SlabConst::kCorePerRunCase;

 private:
  /**
   * @brief construction and destruction operations of thread local cache
   * */
  class TcacheBuilder {
    bool destroyed_;
    ThreadCache* tcache_;

   public:
    explicit TcacheBuilder(Allocator& allocator)
      : destroyed_(false), tcache_(nullptr) {
      ExtentCase* ext_case = allocator.ext_case_;
      RunCase* rcases = allocator.run_cases_;
      Arena* arenas = allocator.arenas_;
      Arena* arena = allocator.choose_arena();
      tcache_ = new ThreadCache(ext_case, allocator.sc_, rcases, arenas, arena);
    }

    TcacheBuilder(const TcacheBuilder&) = delete;

    TcacheBuilder& operator=(const TcacheBuilder&) = delete;

    /**
     * @brief locate thread local cache
     * */
    ThreadCache& locate() { return *tcache_; }

    /**
     * @brief release all regions back to arena
     * */
    void destroy() {
      // avoid main thread cache double destruction/free
      if(!destroyed_) {
        delete tcache_;
        destroyed_ = true;
      }
    }

    ~TcacheBuilder() { destroy(); }
  };

  /**
   * @brief choose arena for thread local cache
   * */
  Arena* choose_arena() {
    int idx = 0;
    for(int aid = 0; aid < narenas_; aid++) {
      // if an arena has no bound threads
      if(arenas_[aid].nbinds() == 0) {
        idx = aid;
        break;
      }
      // find the arena with the least threads bound to
      if(arenas_[aid].nbinds() < arenas_[idx].nbinds()) {
        idx = aid;
      }
    }
    return &arenas_[idx];
  }

  /**
   * @brief get thread cache builder
   * */
  TcacheBuilder& builder() {
    static thread_local TcacheBuilder builder(*this);
    return builder;
  }

 public:
  Allocator() : ext_case_(nullptr), sc_(nullptr), nruns_(-1),
                run_cases_(nullptr), narenas_(-1), arenas_(nullptr) {
    ext_case_ = new ExtentCase();
    sc_ = new SizeClass();
    size_t ncpus = ncpus_online();
    nruns_ = (ncpus + kCorePerRunCase - 1) / kCorePerRunCase;
    run_cases_ = (RunCase*) malloc(sizeof(RunCase) * nruns_);
    for(size_t rid = 0; rid < nruns_; rid++) {
      new(run_cases_ + rid) RunCase(rid, ext_case_, sc_);
    }
    narenas_ = (ncpus + kCorePerArena - 1) / kCorePerArena;
    arenas_ = (Arena*) malloc(sizeof(Arena) * narenas_);
    for(size_t aid = 0; aid < narenas_; aid++) {
      new(arenas_ + aid) Arena(aid, run_cases_ + aid % nruns_, sc_);
    }
  }

  ~Allocator() {
    builder().destroy(); // manually destroy main thread cache
    std::destroy_n(arenas_, narenas_);  // return all used runs back to their corresponding RunCase
    std::destroy_n(run_cases_, nruns_); // return all used extents back to ExtentCase and persist for easy restart
    free(arenas_), free(run_cases_);
    delete sc_, delete ext_case_; // modify and persist allocator state code
  }

  Allocator(const Allocator&) = delete;

  Allocator& operator=(const Allocator&) = delete;

  /**
   * @brief create or open a persistent memory pool
   * @param path pool path
   * @param size pool size in bytes (device capacity by default)
   * */
  void open(const std::string& path, size_t size = -1) {
    if(size == -1) { ext_case_->open(path); }
    else { ext_case_->open(path, size); }
  }

  /**
   * @brief check the state of persistent memory pool; true means the persistent
   * memory pool is ready for allocation; false means the memory pool is closed
   * incorrectly so the meta-info needs to be rebuilt through the iterator
   * */
  bool good() {
    return false;
  }

  /**
   * @brief persistent root
   * */
  PersistRoot& root() { return ext_case_->root(); }

  /**
   * @brief allocate a memory block from persistent memory pool
   * @details only reserve a memory block and init the corresponding
   * token, to use the block whether as a volatile memory block or as
   * a persistent memory block is up to your application scenario
   * */
  region_t acquire(size_t size) {
    ThreadCache& tcache = builder().locate();
    return tcache.acquire(size);
  }

  /**
   * @brief return a memory block back to persistent memory pool and
   * update its state as unallocated
   * */
  void release(void* ptr) {
    ThreadCache& tcache = builder().locate();
    tcache.release(ptr);
  }
};

}

#endif //SLABSTORE_SLAB_H