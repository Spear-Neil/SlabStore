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
#include <functional>
#include <vector>
#include <thread>

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
using util::PinningMap;

class Allocator {
  ExtentCase* ext_case_; // extent manager for extent (de-)allocation, physical page (de-)allocation, etc.
  SizeClass* sc_;        // mutual conversion between cache bin index and slab size
  size_t nruns_;         // the number of RunCase
  RunCase* run_cases_;   // run (de-)allocation & large region (de-)allocation
  size_t narenas_;       // the number of arenas
  Arena* arenas_;        // small & medium region allocation
  bool state_;           // allocator state (whether the allocator is ok for allocation)
  bool recovering;       // in the process of rebuilding allocator metadata and user-defined data structure

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
      // find the arena with the fewest threads bound to
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

  /**
   * @brief reboot a half-used extent for small alloc request
   * @param desc descriptor to the half-used extent
   * */
  void small_reboot(ExtentDesc* desc) { // thread safe
    assert(desc != nullptr && desc->type() == kSmall);
    assert(desc->rcase() < nruns_);
    // first, reload extent back to the corresponding RunCase
    run_cases_[desc->rcase()].small_reload(desc);
    // second, reload half-used runs back to the corresponding Arena
    void* ext = ext_case_->extent(desc);
    size_t rsize = desc->size();
    for(size_t rid = 0; rid < desc->count(); rid++) {
      auto meta = (SmallMeta*) ((uintptr_t) ext + rid * rsize);
      assert(meta->cond() == kPure || meta->cond() == kPolluted);
      if(desc->small_used(rid) && meta->cond() == kPolluted) {
        // runs marked with kPolluted must be half-used
        arenas_[meta->arena()].reload(meta, (void*) meta);
        meta->cond() = kPure;
        persist_write_back(&meta->cond(), sizeof(RunCond));
      }
    }
  }

  /**
   * @brief recover/restore a small extent
   * @param rebuild functor (function object) for rebuild user defined data structure
   * @param desc descriptor to the extent
   * */
  void small_recover(const std::function<void(region_t)>& rebuild, ExtentDesc* desc) { // thread safe
    assert(desc != nullptr && desc->type() == kSmall);
    // first, reload extent back to the corresponding RunCase
    run_cases_[desc->rcase()].small_reload(desc);
    // second, reload used runs and iterate all used regions
    void* ext = ext_case_->extent(desc);
    size_t rsize = desc->size();
    for(size_t rid = 0; rid < desc->count(); rid++) {
      auto meta = (SmallMeta*) ((uintptr_t) ext + rid * rsize);
      void* objs = (void*) ((uintptr_t) meta + rsize - meta->count() * meta->size());
      assert(meta->cond() == kPure || meta->cond() == kPolluted);
      if(desc->small_used(rid)) {
        // reload half-used runs before iterating regions, because rebuild may release a region
        if(meta->half_used()) arenas_[meta->arena()].reload(meta, (void*) meta);
        // processing runs marked with kPolluted (crashes when closing pool)
        if(meta->cond() == kPolluted) {
          meta->cond() = kPure;
          persist_write_back(&meta->cond(), sizeof(RunCond));
        }

        // iterate all used regions for rebuilding user-defined data structure
        std::vector<region_t> regions;
        regions.reserve(meta->count());
        for(size_t ind = 0; ind < meta->count(); ind++) {
          token_t* token = meta->token(ind);
          void* obj = (void*) ((uintptr_t) objs + meta->size() * ind);
          if(token->busy()) regions.emplace_back(token, obj);
        }
        // load all these regions before invoking rebuild, because meta may be changed by other threads
        for(auto reg : regions) rebuild(reg);
      }
    }
  }

  /**
   * @brief reboot a half-used extent for medium alloc request
   * @param desc descriptor to the half-used extent
   * */
  void medium_reboot(ExtentDesc* desc) { // thread safe
    assert(desc != nullptr && desc->type() == kMedium);
    assert(desc->rcase() < nruns_);
    // first, reload extent back to the corresponding RunCase
    run_cases_[desc->rcase()].medium_reload(desc);
    // second, reload half-used runs back to the corresponding Arena
    void* ext = ext_case_->extent(desc);
    size_t rsize = desc->size();
    for(size_t rid = 0; rid < desc->count(); rid++) {
      auto meta = (MediumMeta*) desc->runs() + rid;
      void* run = (void*) ((uintptr_t) ext + rid * rsize);
      assert(meta->cond() == kPure || meta->cond() == kPolluted);
      if(desc->medium_used(rid) && meta->cond() == kPolluted) {
        // runs marked with kPolluted must be half-used
        arenas_[meta->arena()].reload(meta, run);
        meta->cond() = kPure;
        persist_write_back(&meta->cond(), sizeof(RunCond));
      }
    }
  }

  /**
   * @brief recover/restore a medium extent
   * @param rebuild functor (function object) for rebuild user defined data structure
   * @param desc descriptor to the extent
   * */
  void medium_recover(const std::function<void(region_t)>& rebuild, ExtentDesc* desc) { // thread safe
    assert(desc != nullptr && desc->type() == kMedium);
    // first, reload extent back to the corresponding RunCase
    run_cases_[desc->rcase()].medium_reload(desc);
    // second, reload used runs and iterate all used regions
    void* ext = ext_case_->extent(desc);
    size_t rsize = desc->size();
    for(size_t rid = 0; rid < desc->count(); rid++) {
      auto meta = (MediumMeta*) desc->runs() + rid;
      void* run = (void*) ((uintptr_t) ext + rid * rsize);
      assert(meta->cond() == kPure || meta->cond() == kPolluted);
      if(desc->medium_used(rid)) {
        // reload half-used runs before iterating regions, because rebuild may release a region
        if(meta->half_used()) arenas_[meta->arena()].reload(meta, run);
        // processing runs marked with kPolluted (crashes when closing pool)
        if(meta->cond() == kPolluted) {
          meta->cond() = kPure;
          persist_write_back(&meta->cond(), sizeof(RunCond));
        }

        // iterate all used regions for rebuilding user-defined data structure
        std::vector<region_t> regions;
        regions.reserve(meta->count());
        for(size_t ind = 0; ind < meta->count(); ind++) {
          token_t* token = meta->token(ind);
          void* obj = (void*) ((uintptr_t) run + meta->size() * ind);
          if(token->busy()) regions.emplace_back(token, obj);
        }
        // load all these regions before invoking rebuild, because meta may be changed by other threads
        for(auto reg : regions) rebuild(reg);
      }
    }
  }

  /**
   * @brief reboot a half-used extent for large alloc request
   * @param desc descriptor to the half-used extent
   * */
  void large_reboot(ExtentDesc* desc) { // thread safe
    assert(desc != nullptr && desc->type() == kLarge);
    assert(desc->rcase() < nruns_);
    run_cases_[desc->rcase()].large_reload(desc);
  }

  /**
   * @brief recover/restore a large extent
   * @param rebuild functor (function object) for rebuild user defined data structure
   * @param desc descriptor to the extent
   * */
  void large_recover(const std::function<void(region_t)>& rebuild, ExtentDesc* desc) { // thread safe
    assert(desc != nullptr && desc->type() == kLarge);
    // reload half-used runs before iterating regions, because rebuild may release a region
    if(desc->half_used()) run_cases_[desc->rcase()].large_reload(desc);
    // iterate all used regions for rebuilding user-defined data structure
    void* ext = ext_case_->extent(desc);
    std::vector<region_t> regions;
    regions.reserve(desc->count());
    for(size_t ind = 0; ind < desc->count(); ind++) {
      token_t* token = desc->token(ind);
      void* obj = (void*) ((uintptr_t) ext + desc->size() * ind);
      if(token->busy()) regions.emplace_back(token, obj);
    }
    // load all these regions before invoking rebuild, because meta may be changed by other threads
    for(auto reg : regions) rebuild(reg);
  }

 public:
  Allocator() : ext_case_(nullptr), sc_(nullptr), nruns_(-1), run_cases_(nullptr),
                narenas_(-1), arenas_(nullptr), state_(false), recovering(false) {
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
    builder().destroy(); // manually destroy the main thread cache
    std::destroy_n(arenas_, narenas_);  // return all used runs to their corresponding RunCase
    std::destroy_n(run_cases_, nruns_); // return all used extents to ExtentCase and persist for easy restart
    free(arenas_), free(run_cases_);
    delete sc_, delete ext_case_; // modify and persist allocator state code
  }

  Allocator(const Allocator&) = delete;

  Allocator& operator=(const Allocator&) = delete;

  /**
   * @brief create or open a persistent memory pool
   * @param path pool path
   * @param size pool size in bytes (device capacity by default)
   * @param nid the start numa node id which the reload task is bound to
   * */
  void open(const std::string& path, size_t size = -1, size_t nid = 0) {
    PinningMap pin;
    pin.pinning_thread(nid, 0, pthread_self());
    bool reboot;
    if(size == -1) { reboot = ext_case_->open(path); }
    else { reboot = ext_case_->open(path, size); }

    if(reboot) {
      std::vector<ExtentDesc*> extents;
      state_ = ext_case_->reboot(extents);
      if(state_) { // reboot after normal shutdown/exit
        for(ExtentDesc* desc : extents) {
          assert(desc->next() == nullptr);
          // It's unlikely that there are too many half-used extents
          switch(desc->type()) {
            case kSmall:
              small_reboot(desc);
              break;
            case kMedium:
              medium_reboot(desc);
              break;
            case kLarge:
              large_reboot(desc);
              break;
            default:
              fprintf(stderr, "[ERROR]: unknown error, invalid extent type\n");
              exit(EXIT_FAILURE);
          }
        }
      }
    } else { state_ = true; }
  }

  /**
   * @brief check the state of persistent memory pool; true means the persistent
   * memory pool is ready for allocation; false means the memory pool is closed
   * incorrectly so the meta-info needs to be rebuilt
   * */
  bool good() const { return state_; }

  /**
   * @brief call this function to rebuild allocator's metadata and iterate all objects/regions
   * to rebuild user defined data structure after power failure or system crash
   * @param rebuild functor (function object) for rebuild user defined data structure
   * (needs to be thread-safe if nthd is greater than 1)
   * @param nthd thread number for rebuilding allocator's metadata and user defined data structure
   * @param nid the start numa node id which recover threads are bound to
   * */
  void recover(const std::function<void(region_t)>& rebuild, size_t nthd = 1, size_t nid = 0) {
    assert(state_ == false && recovering == false);
    recovering = true;
    std::vector<std::thread> workers;
    PinningMap pin;
    pin.reset_pinning_counter(nid, 0);
    // first, rollback/resume outstanding extent allocation/release transaction
    auto recovery = ext_case_->resume();
    // second, fully scan the whole pool to rebuild allocator and user-defined data structure

    for(int tid = 0; tid < nthd; tid++) {
      workers.push_back(std::thread([&](int tid) {
        pin.pinning_thread_continuous(pthread_self());
        ExtentDesc* desc = recovery.next();
        for(; desc != nullptr; desc = recovery.next()) {
          RegionType type = desc->type();
          if(type == kInvalid) continue; // reclaimed/free extent
          switch(type) {
            case kSmall:
              small_recover(rebuild, desc);
              break;
            case kMedium:
              medium_recover(rebuild, desc);
              break;
            case kLarge:
              large_recover(rebuild, desc);
              break;
            default:
              fprintf(stderr, "[ERROR]: unknown error, invalid extent type\n");
              exit(EXIT_FAILURE);
          }
        }
      }, tid));
    }
    for(int tid = 0; tid < nthd; tid++) {
      workers[tid].join();
    }
    state_ = true, recovering = false;
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
    assert(state_ == true || recovering == true); // call recover for rebuild allocator and user defined data structure
    ThreadCache& tcache = builder().locate();
    return tcache.acquire(size);
  }

  /**
   * @brief return a memory block back to persistent memory pool and update its state as unallocated
   * @details all the regions/objects that are not released back to allocator are regarded as properly
   * used by user defined data structures, only free/released regions are considered for allocator recovery
   * when the allocator is closing/rebooting.
   * */
  void release(void* ptr) {
    assert(state_ == true || recovering == true); // call recover for rebuild allocator and user defined data structure
    ThreadCache& tcache = builder().locate();
    tcache.release(ptr);
  }
};

}

#endif //SLABSTORE_SLAB_H