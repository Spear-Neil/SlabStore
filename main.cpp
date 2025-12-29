#include <iostream>
#include <thread>
#include <string>
#include "../store/hash-store.h"

constexpr size_t BUF_SIZE = 1024;

util::String& kbuf(std::string_view key) {
  static thread_local char buf[BUF_SIZE];
  assert(key.length() + sizeof(util::String) <= sizeof(buf));
  auto kbuf = (util::String*) buf;
  kbuf->len = key.length();
  memcpy(kbuf->str, key.data(), key.length());
  return *kbuf;
}


int main(int argc, char* argv[]) {
  if(argc < 2) {
    std::cout << "[Usage]: store path" << std::endl;
    exit(-1);
  }
  std::string path = std::string(argv[1]);
  size_t store_size = 0x01ul << 30 * 1;

  SlabStore::HashStore<util::String> store;
  store.open(path, store_size);

  /// insert
  std::cout << "\n\nInsert: " << std::endl;
  for(size_t kid = 0; kid < 10; kid++) {
    util::EpochGuard guard(store.get_epoch(), 1);
    std::string key("key:" + std::to_string(kid));
    store.upsert(kbuf(key), &kid, 8);
  }

  for(size_t kid = 0; kid < 10; kid++) {
    util::EpochGuard guard(store.get_epoch(), 1);
    std::string key("key:" + std::to_string(kid));
    auto kv = store.lookup(kbuf(key));
    if(kv == nullptr) {
      std::cerr << "[ERROR]: " << key << " not found!" << std::endl;
    } else {
      std::cout << "[Lookup]: " << key << ", " << *(uint64_t*) (kv->kv + kv->klen) << std::endl;
    }
  }

  /// update
  std::cout << "\n\nUpdate: " << std::endl;
  for(size_t kid = 0; kid < 10; kid++) {
    util::EpochGuard guard(store.get_epoch(), 1);
    std::string key("key:" + std::to_string(kid));
    size_t value = kid + 1;
    store.upsert(kbuf(key), &value, 8);
  }

  for(size_t kid = 0; kid < 10; kid++) {
    util::EpochGuard guard(store.get_epoch(), 1);
    std::string key("key:" + std::to_string(kid));
    auto kv = store.lookup(kbuf(key));
    if(kv == nullptr) {
      std::cerr << "[ERROR]: " << key << " not found!" << std::endl;
    } else {
      std::cout << "[Lookup]: " << key << ", " << *(uint64_t*) (kv->kv + kv->klen) << std::endl;
    }
  }

  /// remove
  std::cout << "\n\nRemove: " << std::endl;
  for(size_t kid = 0; kid < 10; kid++) {
    util::EpochGuard guard(store.get_epoch(), 1);
    std::string key("key:" + std::to_string(kid));
    store.remove(kbuf(key));
  }
  for(size_t kid = 0; kid < 10; kid++) {
    util::EpochGuard guard(store.get_epoch(), 1);
    std::string key("key:" + std::to_string(kid));
    auto kv = store.lookup(kbuf(key));
    if(kv == nullptr) {
      std::cout << "[Remove]: " << key << " has been removed!" << std::endl;
    } else {
      std::cerr << "[ERROR]: " << key << " has not been properly removed!" << std::endl;
    }
  }

  return 0;
}