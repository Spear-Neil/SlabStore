#include "./tree_api.hpp"
#include "utree.h"

class UtreeWrapper : public tree_api {
  btree utree;

 public:
  ~UtreeWrapper() override = default;

  bool find(const char* key, size_t sz, char* value_out) override {
    return utree.search(*(uint64_t*) key, value_out);
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    return utree.insert(*(uint64_t*) key, (char*) value);
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    return utree.insert(*(uint64_t*) key, (char*) value);
  }

  bool remove(const char* key, size_t key_sz) override {
    return utree.remove(*(uint64_t*) key);
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  assert(opt.key_size == 8);
  assert(opt.value_size == 8);
  return new UtreeWrapper();
}