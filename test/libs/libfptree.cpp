#include "tree_api.hpp"
#include "fptree.h"

class FPTreeWrapper : public tree_api {
  FPtree tree;

 public:
  FPTreeWrapper() {
    const char* pool = "/mnt/pmem0/pibench/fptree";
    const size_t size = (0x01ul << 30) * 100;
    tree.pmemInit(pool, size);
  }

  ~FPTreeWrapper() override = default;

  bool find(const char* key, size_t sz, char* value_out) override {
    // For now only support 8 bytes key and value (uint64_t)
    uint64_t value = tree.find(*reinterpret_cast<uint64_t*>(const_cast<char*>(key)));
    if(value == 0) return false;
    memcpy(value_out, &value, sizeof(value));
    return true;
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    KV kv = KV(*(uint64_t*) key, *(uint64_t*) value);
    if(!tree.insert(kv)) {
      return false;
    }
    return true;
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    KV kv = KV(*(uint64_t*) key, *(uint64_t*) value);
    if(!tree.update(kv)) {
      return false;
    }
    return true;
  }

  bool remove(const char* key, size_t key_sz) override {
    if(!tree.deleteKey(*(uint64_t*) key)) {
      return false;
    }
    return true;
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  assert(opt.key_size == 8);
  assert(opt.value_size == 8);
  return new FPTreeWrapper();
}
