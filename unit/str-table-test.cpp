#include <iostream>
#include <random>
#include <vector>
#include <thread>
#include <fstream>
#include "../store/hash-table.h"
#include "util.h"

using namespace util;
using namespace SlabStore;

static size_t nthd = 48;
static const char* path = "/mnt/pmem0/index_workloads/load.dat";
static const size_t load_size = 100'000'000ul;
static const size_t run_duration = 100;

typedef HashTable<String, uint64_t> index_t;

int main(int argc, char* argv[]) {
  PinningMap pin;
  pin.pinning_thread(0, 0, pthread_self());
  if(nthd < pin.processor_number() / pin.numa_number()) {
    pin.set_numa_policy(false);
  } else { pin.set_numa_policy(true); }

  index_t table;
  std::vector<String*> requests;
  requests.reserve(load_size);

  std::cout << "[INFO]: read workloads ... " << std::flush;
  std::ifstream fin(path);
  std::string line;
  while(std::getline(fin, line)) {
    auto row = string_split(line, ' ');
    std::string& key = row.back();
    String* req = String::make_string(key.data(), key.length());
    requests.push_back(req);
    if(requests.size() > load_size) break;
  }
  std::shuffle(requests.begin(), requests.end(), std::mt19937());
  std::cout << "end" << std::endl;

  std::cout << "[INFO]: load phase ... " << std::flush;
  std::vector<std::thread> workers;
  for(size_t tid = 0; tid < nthd; tid++) {
    workers.push_back(std::thread([&](int tid) {
      pin.pinning_thread_continuous(pthread_self());
      for(size_t rid = tid; rid < requests.size(); rid += nthd) {
        String* req = requests[rid];
        auto kv = index_t::KVPair::make_kv(req->str, req->len, rid);
        auto ret = table.upsert(kv);
        if(ret != nullptr) {
          std::cerr << "[ERROR]: insert unknown error" << std::endl;
          exit(-1);
        }
      }
    }, tid));
  }
  for(size_t tid = 0; tid < nthd; tid++) { workers[tid].join(); }
  std::cout << "end" << std::endl;

  std::cout << "[INFO]: load factor: " << (double) table.size() / table.capacity() << std::endl;

  std::cout << "[INFO]: run phase ... " << std::flush;
  workers.clear(), pin.reset_pinning_counter(0, 0);
  for(size_t tid = 0; tid < nthd; tid++) {
    workers.push_back(std::thread([&](int tid) {
      pin.pinning_thread_continuous(pthread_self());
      Timer<> timer;
      timer.start();
      while(true) {
        for(size_t rid = tid; rid < requests.size(); rid += nthd) {
          String* req = requests[rid];
          auto kv = table.lookup(*req);
          if(kv == nullptr || *req != kv->key || kv->value != rid) {
            std::cerr << "[ERROR]: lookup unknown error" << std::endl;
            exit(-2);
          }
        }
        if(timer.duration_s() >= run_duration) break;
      }
    }, tid));
  }
  for(size_t tid = 0; tid < nthd; tid++) { workers[tid].join(); }
  std::cout << "end" << std::endl;

  return 0;
}