#include "tree_api.hpp"
#include "../store/hash-store.h"

constexpr size_t BUF_SIZE = 1024;

class BasicSlabStoreWrapper : public tree_api {
  struct BasicConfig : SlabStore::HashStoreConfig {
    static constexpr bool kWriteOpt = false;
  };

  typedef SlabStore::HashStore<util::String, BasicConfig> DB;
  DB* db;

 private:
  util::String& kbuf(const char* key, size_t klen) {
    static thread_local char buf[BUF_SIZE];
    assert(klen + sizeof(util::String) <= sizeof(buf));
    auto kbuf = (util::String*) buf;
    kbuf->len = klen;
    memcpy(kbuf->str, key, klen);
    return *kbuf;
  }

 public:
  BasicSlabStoreWrapper() {
    const std::string path = "/mnt/pmem0/pibench/slabstore";
    const size_t size = (0x01ul << 30) * 100;
    db = new DB();
    db->open(path, size);
  }

  ~BasicSlabStoreWrapper() override = default;

  bool find(const char* key, size_t sz, char* value_out) override {
    util::EpochGuard guard(db->get_epoch(), 1);
    DB::KVPair* kv = db->lookup(kbuf(key, sz));
    if(kv == nullptr) return false;
    memcpy(value_out, kv->kv + kv->klen, kv->vlen);
    return true;
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    util::EpochGuard guard(db->get_epoch(), 1);
    db->upsert(kbuf(key, key_sz), (void*) value, value_sz);
    return true;
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    util::EpochGuard guard(db->get_epoch(), 1);
    return db->update(kbuf(key, key_sz), (void*) value, value_sz);
  }

  bool remove(const char* key, size_t key_sz) override {
    util::EpochGuard guard(db->get_epoch(), 1);
    return db->remove(kbuf(key, key_sz));
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  return new BasicSlabStoreWrapper();
}