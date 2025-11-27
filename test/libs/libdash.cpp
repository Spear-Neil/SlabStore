#include "./tree_api.hpp"
#include "Hash.h"
#include "allocator.h"
#include "ex_finger.h"


class DashWrapper : public tree_api {
  Hash<uint64_t>* table;
 public:
  DashWrapper() {
    const char* pool = "/mnt/pmem0/pibench/dash";
    const size_t size = (0x01ul << 30) * 200;

    // Step 1: create (if not exist) and open the pool
    bool file_exist = false;
    if(FileExists(pool)) file_exist = true;
    Allocator::Initialize(pool, size);

    // Step 2: Allocate the initial space for the hash table on PM and get the
    // root; we use Dash-EH in this case.
    Hash<uint64_t>* hash_table = reinterpret_cast<Hash<uint64_t>*>(
      Allocator::GetRoot(sizeof(extendible::Finger_EH<uint64_t>)));

    // Step 3: Initialize the hash table
    if(!file_exist) {
      // During initialization phase, allocate 64 segments for Dash-EH
      size_t segment_number = 64;
      new(hash_table) extendible::Finger_EH<uint64_t>(
        segment_number, Allocator::Get()->pm_pool_);
    } else {
      new(hash_table) extendible::Finger_EH<uint64_t>();
    }
    table = hash_table;
  }

  ~DashWrapper() override = default;

  bool find(const char* key, size_t sz, char* value_out) override {
    auto epoch_guard = Allocator::AquireEpochGuard();
    return table->Get(*(uint64_t*) key, (const char**) value_out, true);
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    auto epoch_guard = Allocator::AquireEpochGuard();
    table->Insert(*(uint64_t*) key, value, true);
    return true;
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    return false;
  }

  bool remove(const char* key, size_t key_sz) override {
    auto epoch_guard = Allocator::AquireEpochGuard();
    return table->Delete(*(uint64_t*) key, true);
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  assert(opt.key_size == 8);
  assert(opt.value_size == 8);
  return new DashWrapper();
}