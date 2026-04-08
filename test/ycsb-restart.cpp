#include <iostream>
#include <string>
#include <chrono>

#include "store.h"

int main(int argc, char* argv[]) {
  std::string path = std::string(argv[1]);
  int store_type = SLABKV;
  if(argc > 2) store_type = std::stoi(argv[2]);

  KVStore* store = get_store((STORE_TYPE) store_type);
  auto start = std::chrono::system_clock::now();
  store->recover(path);
  auto end = std::chrono::system_clock::now();
  long duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "YCSB-Restart: Total Recovery time: " << duration << " ms" << std::endl;
  delete store;
}