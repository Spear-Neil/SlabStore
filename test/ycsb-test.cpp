#include <iostream>
#include "store.h"



int main(int argc, char* argv[]) {
  KVStore& store = *get_store(ROCKSKV);

  store.open("/mnt/pmem0/ycsb-kv", 1024 * 1024 * 1024 * 2ul);
  std::string k0 = "key0", k1 = "key1";
  std::string v0 = "value0", v1 = "value1";

  std::string value;
  store.insert(k0, v0);
  store.lookup(k0, value);
  std::cout << "key: " << k0 << ", value: " << value << std::endl;

  store.update(k0, v1);
  store.lookup(k0, value);
  std::cout << "key: " << k0 << ", value: " << value << std::endl;

  return 0;
}