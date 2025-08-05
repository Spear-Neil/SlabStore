#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_set>
#include <vector>

#include "../slab/slab.h"
#include "util.h"

using namespace util;
using namespace SlabStore;

const char words[kPageSize] = "As the quotations that open this chapter show,theviewthatadvancesinuniprocessor\n"
                              " architecture were nearing an endhas beenheldbysomeresearchersformanyyears.\n"
                              " Clearly, these views were premature; in fact, during the period of 1986–2003,\n"
                              " uniprocessor performance growth, driven by the microprocessor, was at its highest\n"
                              " rate since the first transistorized computers in the late 1950s and early 1960s.";

int main() {
  int nthd = 8;

  Allocator allocator;
  allocator.open("/home/sn/pmem/SlabStore/");
//  allocator.open("/mnt/pmem0/SlabStore/");

  size_t max_size = SlabConst::kMaxLargeSize;

  std::vector<std::thread> workers;
  std::vector<std::unordered_set<void*>> objs(nthd);
  std::unordered_set<void*> all_objs(nthd * max_size);
  for(int tid = 0; tid < nthd; tid++) {
    workers.push_back(std::thread([&](int tid) {
      objs[tid].reserve(max_size);
      size_t increment = 32, count = 0;
      for(size_t size = 1; size <= max_size; size += increment) {
        size_t opcnt = max_size / size;
        if(opcnt < 128) opcnt = 128;
        for(size_t i = 0; i < opcnt; i++) {
          region_t reg = allocator.acquire(size);
          assert(objs[tid].find(reg.pointer()) == objs[tid].end());
          objs[tid].insert(reg.pointer());
          assert(reg.pointer() != nullptr);
          size_t ws = size % kPageSize;
          memcpy(reg.pointer(), words, ws);
          persist_write_back(reg.pointer(), ws);
          persist_wait_finish();
        }
        if(++count % 4 == 0) increment <<= 1;
      }
    }, tid));
  }
  size_t total_count = 0;
  for(int tid = 0; tid < nthd; tid++) {
    workers[tid].join();
    total_count += objs[tid].size();
    all_objs.merge(objs[tid]);
  }
  assert(all_objs.size() == total_count);

  return 0;
}