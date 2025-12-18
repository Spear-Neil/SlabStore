#include "tree_api.hpp"
#include "../plush/src/hashtable/Hashtable.h"
#include <cassert>
#include <filesystem>
#include <iostream>

class PlushWrapper : public tree_api {
  typedef Hashtable<std::span<const std::byte>, std::span<const std::byte>, PartitionType::Hash> DB;
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
    db = new DB(path, true, true, 0.4);
  }

  ~PlushWrapper() override = default;

  bool find(const char* key, size_t sz, char* value_out) override {
    return db->lookup(std::span<std::byte>((std::byte*) key, sz), (uint8_t*) value_out);
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    db->insert(std::span<std::byte>((std::byte*) key, key_sz),
               std::span<std::byte>((std::byte*) value, value_sz));
    return true;
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    db->insert(std::span<std::byte>((std::byte*) key, key_sz),
               std::span<std::byte>((std::byte*) value, value_sz));
    return true;
  }

  bool remove(const char* key, size_t key_sz) override {
    db->remove(std::span<std::byte>((std::byte*) key, key_sz));
    return true;
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }

  size_t get_size() override {
    return db->get_pm_size();
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  return new PlushWrapper();
}