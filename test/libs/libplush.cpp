#include "tree_api.hpp"
#include "../plush/src/hashtable/Hashtable.h"
#include <cassert>
#include <filesystem>
#include <iostream>

class PlushWrapper : public tree_api {
  typedef Hashtable<uint64_t, uint64_t, PartitionType::Hash> DB;
  DB* db;

 public:
  PlushWrapper() {
    const std::string path = "/mnt/pmem0/pibench/plush";
    if(std::filesystem::exists(path)) {
      std::filesystem::remove_all(path);
    }
    if(!std::filesystem::create_directory(path)) {
      std::cerr << "[ERROR]: Plush, failed to create dir" << std::endl;
      exit(-1);
    }
    db = new DB(path, true);
  }

  ~PlushWrapper() override = default;

  bool find(const char* key, size_t sz, char* value_out) override {
    return db->lookup(*(uint64_t*) key, (uint8_t*) value_out);
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    db->insert(*(uint64_t*) key, *(uint64_t*) value);
    return true;
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    db->insert(*(uint64_t*) key, *(uint64_t*) value);
    return true;
  }

  bool remove(const char* key, size_t key_sz) override {
    db->insert(*(uint64_t*) key, key_sz, true);
    return true;
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  assert(opt.key_size == 8);
  assert(opt.value_size == 8);
  return new PlushWrapper();
}