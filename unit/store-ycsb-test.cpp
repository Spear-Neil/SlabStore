#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <filesystem>

#include "util.h"
#include "../store/hash-store.h"

using namespace util;
using namespace SlabStore;

constexpr size_t kValueLen = 32; // value length of each record
constexpr size_t run_duration = 10; // run phase duration, second
constexpr bool zipf = true; // requests distribution in run phase, zipfian/uniform
constexpr size_t read_ratio = 100; // ratio of read operations in run phase

constexpr bool crash = false; // abnormal termination

static char common_value[1024]{"Large main memory capacity and even larger data sets have moti\n"
                               "vated hybrid storage systems, which serve most transactions from\n"
                               " memory, but can seamlessly transition to flash storage."};
static_assert(kValueLen <= 1024);
static_assert(read_ratio <= 100);

int main(int argc, char* argv[]) {
  if(argc < 3) {
    std::cerr << "[USAGE]: load path, store path, worker thread number (1 by default),"
                 " recovery thread number (1 by default)" << std::endl;
    exit(EXIT_FAILURE);
  }
  size_t nworker = 1, nrecover = 1;

  std::string load_path(argv[1]);
  std::string store_path(argv[2]);
  if(argc > 3) nworker = std::stoi(argv[3]);
  if(argc > 4) nrecover = std::stoi(argv[4]);

  printf("[INFO]: load path: %s, store path: %s\n", load_path.data(), store_path.data());
  printf("[INFO]: worker thread number: %zu, recover thread number: %zu\n", nworker, nrecover);

  PinningMap pin;
  pin.pinning_thread(0, 0, pthread_self());

  std::ifstream loads(load_path);
  if(!loads.good()) {
    std::cerr << "[ERROR]: failed to open " << load_path << std::endl;
    exit(EXIT_FAILURE);
  }

  std::cout << "[INFO]: read workloads ... " << std::flush;
  std::vector<String*> requests;
  requests.reserve(1000000);
  std::string raw_req;
  while(std::getline(loads, raw_req)) {
    auto&& row = string_split(std::move(raw_req), ' ');
    assert(row.size() == 2 && row[0] == "INSERT");
    std::string& key = row.back();
    auto req = String::make_string(key.data(), key.size());
    requests.push_back(req);
  }
  std::cout << "end, request count: " << requests.size() << std::endl;

  bool exist = std::filesystem::exists(store_path);

  HashStore<String> store;
  store.open(store_path, nrecover);

  if(exist) {
    std::cout << "[INFO]: reboot/recovery verification ... " << std::flush;
    bool find_all = true;
    for(auto& req : requests) {
      auto kv = store.lookup(*req);
      if(kv == nullptr || kv->key != *req) {
        find_all = false;
        break;
      }
    }
    std::cout << "end, all records exist: " << GRAPH_FONT_RED <<
              (find_all ? "yes" : "no") << GRAPH_ATTR_NONE << std::endl;
  }
  std::cout << "[INFO]: number of records: " << store.size() << std::endl;

  std::vector<std::thread> workers;
  std::vector<double> throughput(nworker);
  double total_tpt = 0;

  pin.reset_pinning_counter(0, 0);
  for(size_t tid = 0; tid < nworker; tid++) {
    workers.push_back(std::thread([&](size_t tid) {
      pin.pinning_thread_continuous(pthread_self());
      size_t begin = tid * requests.size() / nworker;
      size_t end = (tid + 1) * requests.size() / nworker;
      Timer timer;
      timer.start();
      for(size_t rid = begin; rid < end; rid++) {
        EpochGuard guard(store.get_epoch());
        store.upsert(*requests[rid], common_value, kValueLen);
      }
      long drt = timer.duration_us();
      throughput[tid] = double(end - begin) / drt;
    }, tid));
  }
  for(size_t tid = 0; tid < nworker; tid++) {
    workers[tid].join();
    total_tpt += throughput[tid];
  }
  std::cout << "[INFO]: load phase throughput: " << total_tpt << std::endl;

  workers.clear(), total_tpt = 0;
  pin.reset_pinning_counter(0, 0);
  for(size_t tid = 0; tid < nworker; tid++) {
    workers.push_back(std::thread([&](size_t tid) {
      pin.pinning_thread_continuous(pthread_self());
      size_t opcnt = 0, rcnt = 0, wcnt = 0;
      UnifGenerator<size_t> op_type(0, 100);
      UnifGenerator<size_t> req_unif(0, requests.size());
      ZipfGenerator<size_t> req_zipf(0, requests.size());
      Timer timer;
      timer.start();

      while(true) {
        bool read = op_type() < read_ratio;
        size_t req_id = zipf ? req_zipf() : req_unif();

        EpochGuard guard(store.get_epoch());
        if(read) {
          auto kv = store.lookup(*requests[req_id]);
          if(kv == nullptr || kv->key != *(requests[req_id])) {
            std::cerr << "[ERROR]: unknown error, records not found" << std::endl;
            exit(EXIT_FAILURE);
          }
          rcnt++;
        } else {
          bool success = store.update(*requests[req_id], common_value, kValueLen);
          if(!success) {
            std::cerr << "[ERROR]: unknown error, records not found" << std::endl;
            exit(EXIT_FAILURE);
          }
          wcnt++;
        }
        if(opcnt++ % 100000 == 0 && timer.duration_s() >= run_duration) break;
      }

      long drt = timer.duration_us();
      throughput[tid] = double(opcnt) / drt;
//      std::cout << tid << " "  << rcnt << " "<< wcnt << std::endl;
    }, tid));
  }
  for(size_t tid = 0; tid < nworker; tid++) {
    workers[tid].join();
    total_tpt += throughput[tid];
  }
  std::cout << "[INFO]: run phase throughput: " << total_tpt << std::endl;
  std::cout << "[INFO]: number of records: " << store.size() << std::endl;

  std::cout << "\n==============================================================\n" << std::endl;

  if(crash) exit(EXIT_FAILURE);

  return 0;
}