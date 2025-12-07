#include "tree_api.hpp"
#include <libpmemkv.hpp>
#include <cstring>

class PmemKVWrapper : public tree_api {
  pmem::kv::db db;

 public:
  PmemKVWrapper() {
    const std::string path = "/mnt/pmem0/pibench/pmemkv";
    const size_t size = (0x01ul << 30) * 100;

    pmem::kv::config cfg;
    cfg.put_string("path", path);
    cfg.put_uint64("create_if_missing", 1);
    cfg.put_uint64("size", size);
    auto status = db.open("cmap", std::move(cfg));
    if(status != pmem::kv::status::OK) {
      std::cerr << "[ERROR]: pmemkv failed to open " << path << std::endl;
      exit(-1);
    }
  }

  ~PmemKVWrapper() override = default;

  bool find(const char* key, size_t sz, char* value_out) override {
    std::string value;
    auto status = db.get(std::string_view(key, sz), &value);

    if(status != pmem::kv::status::OK) return false;
    memcpy(value_out, value.data(), value.size());
    return true;
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    auto status = db.put(std::string_view(key, key_sz), std::string_view(value, value_sz));
    if(status != pmem::kv::status::OK) return false;
    return true;
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    return insert(key, key_sz, value, value_sz);
  }

  bool remove(const char* key, size_t key_sz) override {
    auto status = db.remove(std::string(key, key_sz));
    if(status != pmem::kv::status::OK) return false;
    return true;
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  return new PmemKVWrapper();
}