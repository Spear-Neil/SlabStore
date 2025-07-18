#include <iostream>
#include <tbb/concurrent_unordered_set.h>
#include <tbb/parallel_for.h>

#include "util.h"

using namespace util;

int main(int argc, char* argv[]) {
  size_t nthd = 20;
  size_t count = 100000000;

  tbb::concurrent_unordered_set<size_t> set;
  tbb::task_arena arena(nthd);
  Timer timer;
  timer.start();
  arena.execute([&]() {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, count),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for(size_t i = range.begin(); i < range.end(); i++) {
                          set.insert(i);
                        }
                      });
  });
  double throughput = double(count) / timer.duration_us();
  std::cout << "[INFO]: thread number: " << nthd << ", total insert throughput: " << throughput << std::endl;

  timer.start();
  arena.execute([&]() {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, count),
                      [&](const tbb::blocked_range<size_t>& range) {
                        for(size_t i = range.begin(); i < range.end(); i++) {
                          if(set.find(i) == set.end())
                            exit(EXIT_FAILURE);
                        }
                      });
  });
  throughput = double(count) / timer.duration_us();
  std::cout << "[INFO]: thread number: " << nthd << ", total lookup throughput: " << throughput << std::endl;

  return 0;
}