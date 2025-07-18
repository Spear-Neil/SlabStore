#include <iostream>
#include "../slab/extent.h"

using namespace SlabStore;

int main() {
  ExtentCase man;
  man.open("/home/sn/pmem/SlabStore/");
  auto ext = man.acquire(kSmall, 0, 16 * 1024, false);

  std::cout << ext.first << " " << ext.second << " " << (size_t) ext.second % SlabConst::kExtentSize << std::endl;
  return 0;
}