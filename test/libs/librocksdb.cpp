#include "tree_api.hpp"
#include "rocksdb/db.h"
#include "rocksdb/table.h"

#include <iostream>

class RocksDBWrapper : public tree_api {
  rocksdb::DB* db;

 public:
  RocksDBWrapper() {
    const std::string path = "/mnt/pmem0/pibench/rocksdb";
    const size_t size = (0x01ul << 30) * 100;

    rocksdb::Options options;
    options.create_if_missing = true;
    //  avoid page-fault and page-zeroing overhead on pmem
    options.recycle_dcpmm_sst = true;
    // write WAL with nt-store
    options.env = rocksdb::NewDCPMMEnv(rocksdb::DCPMMEnvOptions(), options.env);
    // key-value separation (allocate values with libpmemobj)
    options.dcpmm_kvs_enable = true;
    options.dcpmm_kvs_mmapped_file_fullpath = path + "/payload";
    options.dcpmm_kvs_mmapped_file_size = size;
    options.dcpmm_kvs_value_thres = 64;
    options.dcpmm_compress_value = false;
    // optimized mmap read for pmem
    options.allow_mmap_reads = true;
    rocksdb::BlockBasedTableOptions bbto;
    bbto.cache_index_and_filter_blocks_for_mmap_read = true;
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbto));

    auto status = rocksdb::DB::Open(options, path, &db);
    if(!status.ok()) {
      std::cerr << "[ERROR]: rocksdb failed to open " << path << std::endl;
      exit(-1);
    }
  }

  bool find(const char* key, size_t sz, char* value_out) override {
    rocksdb::ReadOptions ropt;
    ropt.skip_memtable = false;
    std::string value;
    auto status = db->Get(ropt, std::string_view(key, sz), &value);
    if(!status.ok()) return false;
    memcpy(value_out, value.data(), value.size());
    return true;
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    rocksdb::WriteOptions wopt;
    wopt.sync = true;
    auto status = db->Put(wopt, std::string_view(key, key_sz), std::string_view(value, value_sz));
    if(status.ok()) return true;
    return false;
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    return insert(key, key_sz, value, value_sz);
  }

  bool remove(const char* key, size_t key_sz) override {
    rocksdb::WriteOptions wopt;
    wopt.sync = true;
    auto status = db->Delete(wopt, std::string_view(key, key_sz));
    if(status.ok()) return true;
    return false;
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  return new RocksDBWrapper();
}