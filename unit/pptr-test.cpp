#include <iostream>
#include "../slab/pptr.h"

using namespace SlabStore;

int main() {
  std::cout << "[INFO]: persistent pointer test" << std::endl;
  pptr32_t pptr32;
  pptr64_t pptr64;

  std::cout << "test 0 (construct): " << ((pptr32.load() != nullptr || pptr64.load() != nullptr) ? "fail" : "pass")
            << std::endl;
  pptr32.store(&pptr32), pptr64.store(&pptr64);
  std::cout << "test 1 (self): " << ((pptr32.load() != &pptr32 || pptr64.load() != &pptr64) ? "fail" : "pass")
            << std::endl;
  pptr32.store(&pptr64), pptr64.store(&pptr32);
  std::cout << "test 2 (cross): " << ((pptr32.load() != &pptr64 || pptr64.load() != &pptr32) ? "fail" : "pass")
            << std::endl;
  pptr32.store(nullptr), pptr64.store(nullptr);
  std::cout << "test 3 (nullptr): " << ((pptr32.load() != nullptr || pptr64.load() != nullptr) ? "fail" : "pass")
            << std::endl;
  std::cout << "test 4 (void*): " << ((pptr32 != nullptr || pptr64 != nullptr) ? "fail" : "pass") << std::endl;

  return 0;
}