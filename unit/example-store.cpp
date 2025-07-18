#include <iostream>
#include <map>

#include "../slab/slab.h"
#include "util.h"

using namespace util;
using namespace SlabStore;

typedef KVPair<size_t, size_t> kv_t;
constexpr size_t key_range = 100000;

int main(int argc, char* argv[]) {
  bool recovery = false;
  bool replace = true;

//  std::map<size_t, kv_t*> table;
  std::unordered_map<size_t, kv_t*> table;
  table.reserve(key_range);

  Allocator allocator;
  allocator.open("/home/sn/pmem/SlabStore/");
//  allocator.open("/mnt/pmem0/SlabStore/");
  Timer timer;
  timer.start();
  if(!allocator.good()) {
    recovery = true;
    if(replace) {
      allocator.recover([&](region_t old_obj) {
        assert(old_obj.first->busy() && old_obj.first->mode() == true);
        kv_t* old_kv = (kv_t*) old_obj.second;
        auto new_obj = allocator.acquire(sizeof(kv_t));
        ((kv_t*) new_obj.second)->key = old_kv->key;
        ((kv_t*) new_obj.second)->value = old_kv->value;
        new_obj.first->hire();
        allocator.release(old_obj.second);
        table[((kv_t*) new_obj.second)->key] = (kv_t*) new_obj.second;
      });
    } else {
      allocator.recover([&](region_t reg) {
        assert(reg.first->busy() && reg.first->mode() == true);
        kv_t* kv = (kv_t*) reg.second;
        table[kv->key] = kv;
      });
    }
  }
  long drt = timer.duration_us();
  std::cout << "[INFO]: " << double(key_range) / drt << std::endl;

  if(!recovery) {
    for(size_t key = 0; key < key_range; key++) {
      auto [token, obj] = allocator.acquire(sizeof(kv_t));
      ((kv_t*) obj)->key = key, ((kv_t*) obj)->value = key;
      token->hire();
      table.insert({key, (kv_t*) obj});
    }
  }

  for(size_t key = 0; key < key_range; key++) {
    auto it = table.find(key);
    if(it == table.end()) {
      std::cout << "[INFO]: key " << key << " not found!" << std::endl;
    }
  }

  exit(EXIT_SUCCESS); // abnormal exit, crashes
}