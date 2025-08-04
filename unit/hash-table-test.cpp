#include <iostream>
#include <tbb/parallel_for.h>
#include <tbb/concurrent_hash_map.h>

#include "../store/hash-table.h"
#include "util.h"

using namespace SlabStore;
using namespace util;

int main() {
  size_t nthd = 96, round = 10;
  size_t kv_count = 1'000'000'000;
  size_t table_type = 0;

  HashTable<uint64_t, uint64_t> table;
  tbb::concurrent_hash_map<uint64_t, uint64_t> map;
  typedef HashTable<uint64_t, uint64_t>::KVPair pair;
  tbb::task_arena arena(nthd);
  Timer timer;
  timer.start();
  arena.execute([&]() {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, kv_count),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for(size_t i = range.begin(); i < range.end(); i++) {
                          if(table_type == 0) {
                            auto kv = (pair*) malloc(sizeof(pair));
                            kv->key = i, kv->value = i;
                            table.upsert(kv);
                          } else if(table_type == 1) map.insert({i, i});
                          else { exit(EXIT_FAILURE); }
                        }
                      });
  });
  double throughput = double(kv_count) / timer.duration_us();
  std::cout << "[INFO]: thread number: " << nthd << ", total insert throughput: " << throughput << std::endl;

  timer.start();
  arena.execute([&]() {
    for(size_t r = 0; r < round; r++) {
      tbb::parallel_for(tbb::blocked_range<size_t>(0, kv_count),
                        [&](const tbb::blocked_range<size_t>& range) {
                          for(size_t i = range.begin(); i < range.end(); i++) {
                            if(table_type == 0) {
                              auto kv = table.lookup(i);
                              if(kv == nullptr || kv->key != i)
                                exit(EXIT_FAILURE);
                            } else if(table_type == 1) {
                              tbb::concurrent_hash_map<uint64_t, uint64_t>::const_accessor res;
                              if(!map.find(res, i) || res->first != i) {
                                exit(EXIT_FAILURE);
                              }
                            } else { exit(EXIT_FAILURE); }
                          }
                        });
    }
  });
  throughput = double(kv_count * round) / timer.duration_us();
  std::cout << "[INFO]: thread number: " << nthd << ", total lookup throughput: " << throughput << std::endl;

  return 0;
}