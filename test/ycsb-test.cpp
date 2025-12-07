#include <iostream>
#include <string>
#include <vector>
#include <atomic>

#include "store.h"
#include "cpucounters.h"
#include "util.h"

using namespace SlabStore;
using namespace util;

extern const char* keyload; // random characters for generating workloads
extern const char* payload;

static constexpr size_t kInsertGran = 100; // worker thread get continuous kInsertGran request each time for insertion
static constexpr size_t kNthdLoads = 32;   // thread number for workloads generation
static constexpr size_t kMaxKeySize = 256;
static constexpr size_t kMaxValSize = 4096;

int main(int argc, char* argv[]) {
  if(argc < 12) {
    std::cerr << "[USAGE]: store path, store size (GiB), store type, worker thread number, records number,\n"
                 "         key size (>8, <=256), value size (>8, <=4096), read ratio(0,100), run duration (seconds)\n"
                 "         enable pcm, request distribution (0-unif, 1-zipf), load thread number (same as workers by default)\n"
                 "         script path, zipf skewness (0.99 by default)"
              << std::endl;
    exit(-1);
  }

  std::string store_path(argv[1]);
  size_t store_size = std::stoul(argv[2]);
  STORE_TYPE store_type = (STORE_TYPE) std::stoi(argv[3]);
  size_t nthd = std::stoul(argv[4]); // worker thread number
  size_t records_num = std::stoul(argv[5]);
  size_t key_size = std::stoul(argv[6]);
  size_t val_size = std::stoul(argv[7]);
  size_t read_ratio = std::stoi(argv[8]);
  size_t run_duration = std::stoul(argv[9]);
  bool enable_pcm = std::stoi(argv[10]);
  bool zipf_dis = std::stoi(argv[11]);
  size_t nthd_load = nthd; // threads number for load phase
  if(argc > 12) nthd_load = std::stoul(argv[12]);
  std::string script = "./pm-script.py"; // script for extract total pmem media access (bytes)
  if(argc > 13) script = std::string(argv[13]);
  double zipf_skew = 0.99;
  if(argc > 14) zipf_skew = std::stod(argv[14]);

  if(store_type >= NUM_KVSTORE) {
    std::cerr << "[ERROR]: invalid store type" << std::endl;
    exit(-1);
  }

  if(records_num % kInsertGran != 0) {
    std::cerr << "[ERROR]: records number must be multiple of " << kInsertGran << std::endl;
    exit(-1);
  }

  if(key_size > kMaxKeySize || key_size < 8) {
    std::cerr << "[ERROR]: invalid key size" << std::endl;
    exit(-1);
  }

  if(val_size < 8 || val_size > kMaxValSize) {
    std::cerr << "[ERROR]: invalid value size" << std::endl;
    exit(-1);
  }

  if(read_ratio > 100) {
    std::cerr << "[ERROR]: invalid read ratio" << std::endl;
    exit(-1);
  }

  KVStore& store = *get_store(store_type);
  store_size = store_size * (0x01ul << 30);
  store.open(store_path, store_size);

  PinningMap pin;
  pin.pinning_thread(0, 0, pthread_self());
  if(nthd < pin.processor_number() / pin.numa_number()) {
    pin.set_numa_policy(false);
  } else { pin.set_numa_policy(true); }

  std::vector<std::string>& workloads = *new std::vector<std::string>(records_num); // workload keys

  printf("[INFO]: store path: %s, size: %zu GB, type: %s, worker thread number: %zu\n",
         store_path.data(), store_size, store.store_type().data(), nthd);
  printf("[INFO]: records number: %zu, key size: %zu, value size: %zu, read ratio: %zu\n",
         records_num, key_size, val_size, read_ratio);
  printf("[INFO]: run duration: %zu, enable pcm: %i, request distribution: %s, zipf skew: %f\n",
         run_duration, enable_pcm, zipf_dis ? "zipf" : "unif", zipf_skew);
  fflush(stdout);

  Timer timer;
  timer.start();
  std::vector<std::thread> workers;
  std::cout << "[INFO]: workloads generation ... " << std::flush;
  for(int tid = 0; tid < kNthdLoads; tid++) {
    workers.push_back(std::thread([&](int tid) {
      pin.pinning_thread_continuous(pthread_self());
      std::mt19937_64 record_gen(hash(tid));
      for(size_t rid = tid; rid < records_num; rid += kNthdLoads) {
        uint64_t rec = record_gen();
        std::string key(keyload, key_size - 8);
        key.append((char*) &rec, 8);
        std::shuffle(key.begin(), key.end(), std::mt19937_64(hash(rid)));
        workloads[rid] = std::move(key);
      }
    }, tid));
  }
  for(int tid = 0; tid < kNthdLoads; tid++) {
    workers[tid].join();
  }
  assert(workloads.size() == records_num);
  std::cout << "end, " << timer.duration_s() << " seconds elapsed" << std::endl;
//  std::set<std::string> uniq;
//  for(const auto& item : workloads) uniq.insert(item);
//  std::cout << "[INFO]: uniq size: " << uniq.size() << std::endl;

  pcm::PCM* pcm = nullptr;
  pcm::SystemCounterState before, after;
  if(enable_pcm) {
    pcm = pcm::PCM::getInstance();
    pcm->checkError(pcm->program());
    before = pcm->getSystemCounterState();
  }

  std::cout << "\n" << "[INFO]: load phase ... " << std::flush;
  timer.start();
  workers.clear(), pin.reset_pinning_counter(0, 0);
  std::vector<double> throughputs(std::max(nthd, nthd_load));
  double load_tpt = 0;
  std::atomic<size_t> inserted = 0;
  for(int tid = 0; tid < nthd_load; tid++) {
    workers.push_back(std::thread([&](int tid) {
      pin.pinning_thread_continuous(pthread_self());
      size_t block_idx = inserted.fetch_add(kInsertGran);

      Timer timer;
      timer.start();
      size_t processed = 0;
      while(block_idx < records_num) {
        for(size_t rid = block_idx; rid < block_idx + kInsertGran; rid++) {
          std::string_view value(payload, val_size);
          store.insert(workloads[rid], value);
        }
        processed += kInsertGran;
        block_idx = inserted.fetch_add(kInsertGran);
      }
      long drt = timer.duration_us();

      throughputs[tid] = (double) processed / drt;
    }, tid));
  }
  for(int tid = 0; tid < nthd_load; tid++) {
    workers[tid].join();
    load_tpt += throughputs[tid];
  }
  long drt = timer.duration_us();
  std::cout << "end, throughput: " << load_tpt << std::endl;

  if(enable_pcm) {
    after = pcm->getSystemCounterState();
    std::cout << "[INFO]: L3 Miss Ratio: " << 1 - pcm::getL3CacheHitRatio(before, after) << std::endl;

    double mem_reads = (double) pcm::getBytesReadFromMC(before, after) / (0x01ul << 20);
    double mem_writes = (double) pcm::getBytesWrittenToMC(before, after) / (0x01ul << 20);
    std::cout << "[INFO]: Mem Reads: " << mem_reads << " MiB, " << mem_reads * 1000000 / drt << " MiB/S" << std::endl;
    std::cout << "[INFO]: Mem Writes: " << mem_writes << " MiB, " << mem_writes * 1000000 / drt << " MiB/S"
              << std::endl;

    double pmem_reads = (double) pcm::getBytesReadFromPMM(before, after) / (0x01ul << 20);
    double pmem_writes = (double) pcm::getBytesWrittenToPMM(before, after) / (0x01ul << 20);
    std::cout << "[INFO]: PMM Reads: " << pmem_reads << " MiB, " << pmem_reads * 1000000 / drt << " MiB/S" << std::endl;
    std::cout << "[INFO]: PMM Writes: " << pmem_writes << " MiB, " << pmem_writes * 1000000 / drt << " MiB/S"
              << std::endl;

    before = pcm->getSystemCounterState();

    int result = system("ipmctl show -performance TotalMediaReads > media-reads.before");
    if(result != 0) {
      std::cerr << "ipmctl error, " << result << std::endl;
      exit(result);
    }
    result = system("ipmctl show -performance TotalMediaWrites > media-writes.before");
    if(result != 0) {
      std::cerr << "ipmctl error, " << result << std::endl;
      exit(result);
    }
  }


  std::cout << "\n" << "[INFO]: run phase ... " << std::flush;
  timer.start();
  workers.clear(), pin.reset_pinning_counter(0, 0);
  double run_tpt = 0;
  std::atomic<size_t> total_failed = 0, total_count = 0;
  for(int tid = 0; tid < nthd; tid++) {
    workers.push_back(std::thread([&](int tid) {
      pin.pinning_thread_continuous(pthread_self());
      UnifGenerator<size_t> op_gen(0, 100, hash(tid));
      UnifGenerator<size_t> req_unif(0, records_num, hash(tid));
      ZipfGenerator<size_t> req_zipf(0, records_num, hash(tid), zipf_skew);

      size_t opcnt = 0, rcnt = 0, wcnt = 0, fails = 0;
      Timer timer;
      timer.start();
      while(true) {
        bool read = op_gen() < read_ratio;
        size_t req = zipf_dis ? req_zipf() : req_unif();

        if(read) {  // lookup
          std::string value;
          value.reserve(val_size);
          bool found = store.lookup(workloads[req], value);
          if(!found) {
            fails++;
//            std::cerr << "[ERROR]: records not found" << std::endl;
//            exit(-1);
          }
          rcnt++;
        } else { // update
          std::string_view value(payload, val_size);
          store.update(workloads[req], value);
          wcnt++;
        }

        if(opcnt++ % 100000 == 0 && timer.duration_s() >= run_duration) break;
      }
      long drt = timer.duration_us();
//      std::cout << "tid: " << tid << ", read/write count: " << rcnt << " / " << wcnt << std::endl;
      total_failed += fails, total_count += opcnt;
      throughputs[tid] = double(opcnt) / drt;
    }, tid));
  }
  for(int tid = 0; tid < nthd; tid++) {
    workers[tid].join();
    run_tpt += throughputs[tid];
  }
  drt = timer.duration_us();
  std::cout << "end, throughput: " << run_tpt << std::endl;
  std::cout << "[INFO]: total failed lookup count: " << total_failed
            << ", total operation count: " << total_count << std::endl;

  if(enable_pcm) {
    after = pcm->getSystemCounterState();
    std::cout << "[INFO]: L3 Miss Ratio: " << 1 - pcm::getL3CacheHitRatio(before, after) << std::endl;

    double mem_reads = (double) pcm::getBytesReadFromMC(before, after) / (0x01ul << 20);
    double mem_writes = (double) pcm::getBytesWrittenToMC(before, after) / (0x01ul << 20);
    std::cout << "[INFO]: Mem Reads: " << mem_reads << " MiB, " << mem_reads * 1000000 / drt << " MiB/S" << std::endl;
    std::cout << "[INFO]: Mem Writes: " << mem_writes << " MiB, " << mem_writes * 1000000 / drt << " MiB/S"
              << std::endl;

    double pmem_reads = (double) pcm::getBytesReadFromPMM(before, after) / (0x01ul << 20);
    double pmem_writes = (double) pcm::getBytesWrittenToPMM(before, after) / (0x01ul << 20);
    std::cout << "[INFO]: PMM Reads: " << pmem_reads << " MiB, " << pmem_reads * 1000000 / drt << " MiB/S" << std::endl;
    std::cout << "[INFO]: PMM Writes: " << pmem_writes << " MiB, " << pmem_writes * 1000000 / drt << " MiB/S"
              << std::endl;

    pcm->cleanup();

    int result = system("ipmctl show -performance TotalMediaReads > media-reads.after");
    if(result != 0) {
      std::cerr << "ipmctl error, " << result << std::endl;
      exit(result);
    }
    result = system("ipmctl show -performance TotalMediaWrites > media-writes.after");
    if(result != 0) {
      std::cerr << "ipmctl error, " << result << std::endl;
      exit(result);
    }


    std::string script_read = std::string("python ") + script +
                              " ./media-reads.before ./media-reads.after TotalMediaReads";
    result = system(script_read.data());
    if(result != 0) {
      std::cerr << "python script error" << std::endl;
      exit(result);
    }
    std::string script_write = std::string("python ") + script +
                               " ./media-writes.before ./media-writes.after TotalMediaWrites";
    result = system(script_write.data());
    if(result != 0) {
      std::cerr << "python script error" << std::endl;
      exit(result);
    }
  }

  return 0;
}


const char* keyload = "This book describes programming techniques for writing applications that use persistent \n"
                      "memory. It is written for experienced software developers, but we assume no previous \n"
                      "experience using persistent memory. We provide many code examples in a variety of \n"
                      "programming languages.";

const char* payload = "Platform vendors such as Intel, AMD, ARM, and others will decide how persistent \n"
                      "memory should be implemented at the lowest hardware levels. We try to provide a \n"
                      "vendor-agnostic perspective and only occasionally call out platform-specific details.\n"
                      " For systems with persistent memory, failure atomicity guarantees that systems can \n"
                      "always recover to a consistent state following a power or system failure. Failure atomicity \n"
                      "for applications can be achieved using logging, flushing, and memory store barriers that \n"
                      "order such operations. Logging, either undo or redo, ensures atomicity when a failure \n"
                      "interrupts the last atomic operation from completion. Cache flushing ensures that \n"
                      "data held within volatile caches reach the persistence domain so it will not be lost if a \n"
                      "sudden failure occurs. Memory store barriers, such as an SFENCE operation on the x86 \n"
                      "architecture, help prevent potential reordering in the memory hierarchy, as caches and \n"
                      "memory controllers may reorder memory operations. For example, a barrier ensures \n"
                      "that the undo log copy of the data gets persisted onto the persistent memory before the \n"
                      "actual data is modified in place. This guarantees that the last atomic operation can be \n"
                      "rolled back should a failure occur. However, it is nontrivial to add such failure atomicity \n"
                      "in user applications with low-level operations such as write logging, cache flushing, and \n"
                      "barriers. The Persistent Memory Development Kit (PMDK) was developed to isolate \n"
                      "developers from having to re-implement the hardware intricacies.\n"
                      " Failure atomicity should be a familiar concept, since most file systems implement \n"
                      "and perform journaling and flushing of their metadata to storage devices."
                      "We use load and store operations to read and write to persistent memory rather than \n"
                      "using block-based I/O to read and write to traditional storage. We suggest reading the \n"
                      "CPU architecture documentation for an in-depth description because each successive \n"
                      "CPU generation may introduce new features, methods, and optimizations.\n"
                      " Using the Intel architecture as an example, a CPU cache typically has three \n"
                      "distinct levels: L1, L2, and L3. The hierarchy makes references to the distance \n"
                      "from the CPU core, its speed, and size of the cache. The L1 cache is closest to \n"
                      "the CPU. It is extremely fast but very small. L2 and L3 caches are increasingly \n"
                      "larger in capacity, but they are relatively slower. Figure 2-1 shows a typical CPU \n"
                      "microarchitecture with three levels of CPU cache and a memory controller with \n"
                      "three memory channels. Each memory channel has a single DRAM and persistent \n"
                      "memory attached. On platforms where the CPU caches are not contained within \n"
                      "the power-fail protected domain, any modified data within the CPU caches that has \n"
                      "not been flushed to persistent memory will be lost when the system loses power or \n"
                      "crashes.  Platforms that do include CPU caches in the power-fail protected domain \n"
                      "will ensure modified data within the CPU caches are flushed to the persistent \n"
                      "memory should the system crash or loses power. We describe these requirements \n"
                      "and features in the upcoming “Power-Fail Protected Domains” section. "
                      " The L1 (Level 1) cache is the fastest memory in a computer system. In terms of access \n"
                      "priority, the L1 cache has the data the CPU is most likely to need while completing a \n"
                      "specific task. The L1 cache is also usually split two ways, into the instruction cache (L1 I)  \n"
                      "and the data cache (L1 D). The instruction cache deals with the information about the \n"
                      "operation that the CPU has to perform, while the data cache holds the data on which the \n"
                      "operation is to be performed.\n"
                      " The L2 (Level 2) cache has a larger capacity than the L1 cache, but it is slower. L2 \n"
                      "cache holds data that is likely to be accessed by the CPU next. In most modern CPUs, \n"
                      "the L1 and L2 caches are present on the CPU cores themselves, with each core getting \n"
                      "dedicated caches.\n"
                      " The L3 (Level 3) cache is the largest cache memory, but it is also the slowest of the \n"
                      "three. It is also a commonly shared resource among all the cores on the CPU and may be \n"
                      "internally partitioned to allow each core to have dedicated L3 resources.";