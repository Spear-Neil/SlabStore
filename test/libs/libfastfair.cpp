#include "tree_api.hpp"
#include "btree.h"

class FastFairWrapper : public tree_api {
  TOID(btree) tree = TOID_NULL(btree);
  PMEMobjpool* pop = nullptr;

 public:
  FastFairWrapper() {
    const char* path = "/mnt/pmem0/pibench/fastfair";
    const size_t size = (0x01ul << 30) * 100;

    if(access(path, F_OK) != 0) {
      pop = pmemobj_create(path, "fast+fair", size, 0666);
      tree = POBJ_ROOT(pop, btree);
        D_RW(tree)->constructor(pop);
    } else {
      pop = pmemobj_open(path, "fast+fair");
      tree = POBJ_ROOT(pop, btree);
    }
  }

  ~FastFairWrapper() override {
    pmemobj_close(pop);
  };

  bool find(const char* key, size_t sz, char* value_out) override {
    *(char**) value_out = D_RW(tree)->btree_search(*(uint64_t*) key);
    return true;
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
      D_RW(tree)->btree_insert(*(uint64_t*) key, (char*) value);
    return true;
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    return false;
  }

  bool remove(const char* key, size_t key_sz) override {
      D_RW(tree)->btree_delete(*(uint64_t*) key);
    return true;
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  assert(opt.key_size == 8);
  assert(opt.value_size == 8);
  return new FastFairWrapper();
}