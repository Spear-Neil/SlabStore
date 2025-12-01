#include "tree_api.hpp"
#include "lbtree.h"

class LBTreeWrapper : public tree_api {
  lbtree* tree;
  std::atomic<size_t> thd_cnt{0};

  void get_worker_id() {
    static thread_local int wid = thd_cnt++ % worker_thread_num;
    worker_id = wid;
  }

 public:
  LBTreeWrapper() {
    worker_thread_num = 48; // up to 48 threads
    get_worker_id(); // main thread use worker[0]'s mem/nvm pool
    size_t mem_size = (0x01ul << 30) * 24; // 24 GiB reserved
    // initialize mempool per worker thread
    the_thread_mempools.init(worker_thread_num, mem_size, 4096);
    size_t nvm_size = (0x01ul << 30) * 100; // 100GB pmem
    const char* pool = "/mnt/pmem0/pibench/lbtree";
    // initialize nvm pool and log per worker thread
    the_thread_nvmpools.init(worker_thread_num, pool, nvm_size);
    // allocate a 4KB page for the tree in worker 0's pool
    char* nvm_addr = (char*) nvmpool_alloc(4 * KB);
    tree = new lbtree(nvm_addr, false);
    // log may not be necessary for some tree implementations
    // For simplicity, we just initialize logs.  This cost is low.
    nvmLogInit(worker_thread_num);
//    printf("lbtree init end\n");
  }

  ~LBTreeWrapper() override = default;

  bool find(const char* key, size_t sz, char* value_out) override {
    get_worker_id();
    int pos;
    void* p = tree->lookup(*(uint64_t*) key, &pos);
    *(void**) value_out = tree->get_recptr(p, pos);
    return true;
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
//    printf("insert key %zu\n begin", *(uint64_t*) key);
    get_worker_id();
    tree->insert(*(uint64_t*) key, *(void**) value);
//    printf("insert key %zu\n end", *(uint64_t*) key);
    return true;
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    return false;
  }

  bool remove(const char* key, size_t key_sz) override {
    get_worker_id();
    tree->del(*(uint64_t*) key);
    return true;
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  assert(opt.key_size == 8);
  assert(opt.value_size == 8);
  return new LBTreeWrapper();
}
