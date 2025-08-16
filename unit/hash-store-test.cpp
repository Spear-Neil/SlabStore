#include "../store/hash-store.h"

#include <iostream>

using namespace SlabStore;

int main() {
  HashStore<uint64_t, OptRowValue> store;
  store.open("/home/sn/pmem/HashStore");

  return 0;
}