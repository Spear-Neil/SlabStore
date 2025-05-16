#include <iostream>
#include <cstring>
#include "../slab/persist.h"

using namespace SlabStore;

int main() {
  std::cout << "[INFO]: persist_state interface test" << std::endl;
  void* block = aligned_alloc(kCacheLineSize, kPageSize);

  memset(block, 0, kPageSize);
  persist_write_back(block, kCacheLineSize);
  persist_write_back(block, 2 * kCacheLineSize);
  persist_write_back((char*) block + 1, kCacheLineSize);

  memset(block, 0, kPageSize);
  persist_serial_flush(block, kCacheLineSize);
  persist_serial_flush(block, 2 * kCacheLineSize);
  persist_serial_flush((char*) block + 1, kCacheLineSize);

  memset(block, 0, kPageSize);
  persist_concur_flush(block, kCacheLineSize);
  persist_concur_flush(block, 2 * kCacheLineSize);
  persist_concur_flush((char*) block + 1, kCacheLineSize);

  persist_wait_finish();

  std::cout << "[INFO]: test pass" << std::endl;

  return 0;
}