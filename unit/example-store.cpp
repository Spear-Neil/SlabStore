#include <iostream>
#include <map>

#include "../slab/slab.h"
#include "util.h"

using namespace util;
using namespace SlabStore;

static constexpr size_t kValueLen = 128;
struct Value { char vstr[kValueLen]; };

typedef KVPair<size_t, Value> kv_t;

constexpr size_t key_range = 10000000;

static char common_value[1024]{"Large main memory capacity and even larger data sets have moti\n"
                               "vated hybrid storage systems, which serve most transactions from\n"
                               " memory, but can seamlessly transition to flash storage."};
static_assert(kValueLen <= 1024);

int main(int argc, char* argv[]) {
  bool recovery = false;
  bool replace = true; // test for allocation during recovery

//  std::map<size_t, kv_t*> table;
  std::unordered_map<size_t, kv_t*> table;
  table.reserve(key_range);

  Allocator allocator;
//  allocator.open("/home/sn/pmem/SlabStore/");
  allocator.open("/mnt/pmem0/SlabStore/");
  Timer timer;
  if(!allocator.good()) {
    timer.start();
    recovery = true;
    if(replace) {
      allocator.recover([&](region_t old_obj) {
        assert(old_obj.busy() && old_obj.mode() == true);
        kv_t* old_kv = (kv_t*) old_obj.pointer();
        auto new_obj = allocator.acquire(sizeof(kv_t));
        ((kv_t*) new_obj.pointer())->key = old_kv->key;
        ((kv_t*) new_obj.pointer())->value = old_kv->value;
        wait_write_back(new_obj.pointer(), sizeof(kv_t));
        new_obj.publish();
        allocator.release(old_obj.pointer());
        table[((kv_t*) new_obj.pointer())->key] = (kv_t*) new_obj.pointer();
      });
    } else {
      allocator.recover([&](region_t reg) {
        assert(reg.busy() && reg.mode() == true);
        kv_t* kv = (kv_t*) reg.pointer();
        table[kv->key] = kv;
      });
    }
    long drt = timer.duration_us();
    std::cout << "[INFO]: recover tpt: " << double(key_range) / drt << std::endl;
  }

  if(!recovery) {
    timer.start();
    for(size_t key = 0; key < key_range; key++) {
      auto obj = allocator.acquire(sizeof(kv_t));
      ((kv_t*) obj.pointer())->key = key;
      memcpy(((kv_t*) obj.pointer())->value.vstr, common_value, kValueLen);
      persist_write_back(obj, sizeof(kv_t));
      persist_wait_finish();
      obj.publish();
      table.insert({key, (kv_t*) obj.pointer()});
    }
    long drt = timer.duration_us();
    std::cout << "[INFO]: insert tpt: " << double(key_range) / drt << std::endl;
  }

  timer.start();
  for(size_t key = 0; key < key_range; key++) {
    auto it = table.find(key);
    if(it == table.end()) {
      std::cout << "[INFO]: key " << key << " not found!" << std::endl;
    } else if(it->second->key != key) {
      std::cout << "[INFO]: key " << key << " not found!" << std::endl;
    }
  }
  long drt = timer.duration_us();
  std::cout << "[INFO]: lookup tpt: " << double(key_range) / drt << std::endl;

  exit(EXIT_SUCCESS); // abnormal exit, crashes
}