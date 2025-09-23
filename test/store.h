/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_STORE_H
#define SLABSTORE_STORE_H

#include <string>
#include <filesystem>
#include <mutex>

#include <libpmemkv.hpp>

#include "../store/hash-store.h"
#include "./plush/src/hashtable/Hashtable.h"
#include "./viper/include/viper/viper.hpp"
#include "rocksdb/db.h"
#include "rocksdb/table.h"

constexpr size_t BUF_SIZE = 1024;

enum STORE_TYPE { PMEMKV = 0, SLABKV, PLUSHKV, VIPERKV, ROCKSKV };

class KVStore {
 public:
  virtual ~KVStore() = default;

  virtual std::string store_type() = 0;

  virtual void open(const std::string& path, size_t size) = 0;

  virtual void insert(std::string_view key, std::string_view value) = 0;

  virtual void update(std::string_view key, std::string_view value) = 0;

  virtual bool lookup(std::string_view key, std::string& value) = 0;
};


class PMemKVStore : public KVStore {
  pmem::kv::db db_;

 public:
  PMemKVStore() = default;

  ~PMemKVStore() = default;

  std::string store_type() override { return "pmemkv"; }

  void open(const std::string& path, size_t size) override {
    pmem::kv::config cfg;
    cfg.put_string("path", path);
    cfg.put_uint64("create_if_missing", 1);
    cfg.put_uint64("size", size);
    auto status = db_.open("cmap", std::move(cfg));
    if(status != pmem::kv::status::OK) {
      std::cerr << "[ERROR]: pmemkv failed to open " << path << std::endl;
      exit(-1);
    }
  }

  void insert(std::string_view key, std::string_view value) override {
    auto status = db_.put(key, value);
    if(status != pmem::kv::status::OK) {
      std::cerr << "[ERROR]: pmemkv failed to insert" << key << std::endl;
      exit(-1);
    }
  }

  void update(std::string_view key, std::string_view value) override {
    insert(key, value);
  }

  bool lookup(std::string_view key, std::string& value) override {
    auto status = db_.get(key, &value);

    if(status == pmem::kv::status::OK) return true;
    if(status == pmem::kv::status::NOT_FOUND) return false;

    std::cerr << "[ERROR]: pmemkv unknown error in lookup" << std::endl;
    exit(-1);
  }
};


class SlabKVStore : public KVStore {
  typedef SlabStore::HashStore<util::String> DB;
  DB* db_;

 private:
  util::String& kbuf(std::string_view key) {
    static thread_local char buf[BUF_SIZE];
    assert(key.length() + sizeof(util::String) <= sizeof(buf));
    auto kbuf = (util::String*) buf;
    kbuf->len = key.length();
    memcpy(kbuf->str, key.data(), key.length());
    return *kbuf;
  }

 public:
  SlabKVStore() : db_(new DB()) {}

  ~SlabKVStore() { delete db_; }

  std::string store_type() override { return "SlabStore"; }

  void open(const std::string& path, size_t size) override {
    db_->open(path, size);
  }

  void insert(std::string_view key, std::string_view value) override {
    db_->upsert(kbuf(key), (void*) value.data(), value.length());
  }

  void update(std::string_view key, std::string_view value) override {
    bool find = db_->update(kbuf(key), (void*) value.data(), value.length());
    if(!find) {
      std::cerr << "[ERROR]: SlabStore try to update an non-existent record" << std::endl;
      exit(-1);
    }
  }

  bool lookup(std::string_view key, std::string& value) override {
    DB::KVPair* kv = db_->lookup(kbuf(key));
    if(kv == nullptr) return false;

    value.assign(kv->kv + kv->key.len, kv->vlen);
    return true;
  }
};


class PlushKVStore : public KVStore {
  typedef Hashtable<std::span<const std::byte>, std::span<const std::byte>, PartitionType::Hash> DB;
  DB* db_;

 public:
  PlushKVStore() : db_(nullptr) {}

  ~PlushKVStore() { delete db_; }

  std::string store_type() override { return "Plush"; }

  void open(const std::string& path, size_t size) override {
    if(std::filesystem::exists(path)) {
      std::filesystem::remove_all(path);
    }
    if(!std::filesystem::create_directory(path)) {
      std::cerr << "[ERROR]: Plush, failed to create dir" << std::endl;
      exit(-1);
    }
    db_ = new DB(path, true);
  }

  void insert(std::string_view key, std::string_view value) override {
    db_->insert(std::span<std::byte>((std::byte*) key.data(), key.size()),
                std::span<std::byte>((std::byte*) value.data(), value.size()));
  }

  void update(std::string_view key, std::string_view value) override { insert(key, value); }

  bool lookup(std::string_view key, std::string& value) override {
    return db_->lookup(std::span<std::byte>((std::byte*) key.data(), key.size()), value);
  }
};


/// only used for benchmark, don't instantiate this twice
class ViperKVStore : public KVStore {
  typedef viper::Viper<std::string, std::string> DB;
  std::unique_ptr<DB> db_;
  std::map<pthread_t, std::unique_ptr<DB::Client>> clients;

 private:
  DB::Client& get_client() {
    static std::mutex lock;
    static thread_local DB::Client* client = nullptr;

    if(client == nullptr) {
      client = new DB::Client(db_->get_client());
      std::lock_guard guard(lock);
      std::unique_ptr<DB::Client> uclient(client);
      clients[pthread_self()] = std::move(uclient);
    }
    return *client;
  }

 public:
  ViperKVStore() = default;

  ~ViperKVStore() = default;

  std::string store_type() { return "Viper"; }

  void open(const std::string& path, size_t size) override {
    viper::ViperConfig config{.enable_reclamation = true};
    db_ = DB::create(path, size, config);
  }

  void insert(std::string_view key, std::string_view value) override {
    get_client().put(std::string(key), std::string(value));
  }

  void update(std::string_view key, std::string_view value) override {
    insert(key, value);
  }

  bool lookup(std::string_view key, std::string& value) override {
    return get_client().get(std::string(key), &value);
  }
};


class RocksKVStore : public KVStore {
  rocksdb::DB* db_;

 public:
  RocksKVStore() : db_(nullptr) {}

  ~RocksKVStore() { delete db_; }

  std::string store_type() override { return "RocksDB"; }

  void open(const std::string& path, size_t size) override {
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

    auto status = rocksdb::DB::Open(options, path, &db_);
    if(!status.ok()) {
      std::cerr << "[ERROR]: rocksdb failed to open " << path << std::endl;
      exit(-1);
    }
  }

  void insert(std::string_view key, std::string_view value) override {
    rocksdb::WriteOptions wopt;
    wopt.sync = true;
    auto status = db_->Put(wopt, key, value);
    if(!status.ok()) {
      std::cerr << "[ERROR]: rocksdb failed to insert/update" << std::endl;
      exit(-1);
    }
  }

  void update(std::string_view key, std::string_view value) override {
    insert(key, value);
  }

  bool lookup(std::string_view key, std::string& value) override {
    rocksdb::ReadOptions ropt;
    ropt.skip_memtable = false;
    auto status = db_->Get(ropt, key, &value);
    if(status.ok()) return true;
    std::cout << status.ToString() << std::endl;
    return false;
  }
};

KVStore* get_store(STORE_TYPE type) {
  switch(type) {
    case PMEMKV:
      return new PMemKVStore();
    case SLABKV:
      return new SlabKVStore();
    case PLUSHKV:
      return new PlushKVStore();
    case VIPERKV:
      return new ViperKVStore();
    case ROCKSKV:
      return new RocksKVStore();
    default:
      std::cerr << "[ERROR]: unknown store type" << std::endl;
      exit(-1);
  }
}

#endif //SLABSTORE_STORE_H
