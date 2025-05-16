#include <iostream>
#include <thread>
#include <vector>
#include <set>

#include "../slab/slab.h"
#include "util.h"

using namespace util;
using namespace SlabStore;

int main(int argc, char* argv[]) {
  Allocator allocator;
  allocator.open("/home/sn/pmem/SlabStore/");
//  allocator.open("/mnt/pmem0/SlabStore/");

  for(size_t slab_ind = 0; slab_ind < SlabConst::kNSlabsCached; slab_ind++) {
    std::set<region_t> objects;
    size_t obj_count = 10000 / (slab_ind + 1);
    size_t obj_size = SizeClass().index2size(slab_ind);

    for(size_t i = 0; i < obj_count; i++) {
      region_t obj = allocator.acquire(obj_size);
      objects.insert(obj);
    }
    for(auto& obj : objects) {
      allocator.release(obj.second);
    }

    objects.clear();
    for(size_t i = 0; i < obj_count; i++) {
      region_t obj = allocator.acquire(obj_size);
      objects.insert(obj);
    }
    for(auto& obj : objects) {
      allocator.release(obj.second);
    }
  }

  for(size_t obj_size = 128 * 1024ul; obj_size <= SlabConst::kMaxLargeSize; obj_size <<= 1) {
    std::set<region_t> objects;
    size_t obj_count = 100;

    for(size_t i = 0; i < obj_count; i++) {
      region_t obj = allocator.acquire(obj_size);
      objects.insert(obj);
    }
    for(auto& obj : objects) {
      allocator.release(obj.second);
    }

    objects.clear();
    for(size_t i = 0; i < obj_count; i++) {
      region_t obj = allocator.acquire(obj_size);
      objects.insert(obj);
    }
    for(auto& obj : objects) {
      allocator.release(obj.second);
    }
  }

  return 0;
}