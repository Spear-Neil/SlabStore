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
  if(!allocator.good()) {
    allocator.recover([&](region_t reg) {
      allocator.release(reg.pointer());
    });
  } else {
    for(size_t slab_ind = 0; slab_ind < SlabConst::kNSlabsCached; slab_ind++) {
      size_t obj_count = 10000 / (slab_ind + 1);
      size_t obj_size = SizeClass().index2size(slab_ind);
      for(size_t i = 0; i < obj_count; i++) {
        region_t obj = allocator.acquire(obj_size);
        obj.publish();
      }
    }

    for(size_t obj_size = 128 * 1024ul; obj_size <= SlabConst::kMaxLargeSize; obj_size <<= 1) {
      size_t obj_count = 100;
      for(size_t i = 0; i < obj_count; i++) {
        region_t obj = allocator.acquire(obj_size);
        obj.publish();
      }
    }

    exit(EXIT_FAILURE);
  }

  return 0;
}