#include <iostream>
#include <string>

#include "../store/hash-store.h"

int main(int argc, char* argv[]) {
  std::string path = std::string(argv[1]);

  SlabStore::HashStore<util::String> store;
  store.open(path, -1, 48, 0);
}