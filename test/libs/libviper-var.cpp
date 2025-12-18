#include "tree_api.hpp"
#include "../viper/include/viper/viper.hpp"
#include <map>
#include <string>

class ViperWrapper : public tree_api {
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
  ViperWrapper(size_t size) {
    const std::string path = "/mnt/pmem0/pibench/viper";
    if(size == 0) size = (0x01ul << 30) * 100;
    viper::ViperConfig config{.enable_reclamation = true};
    db_ = DB::create(path, size, config);
  }

  ~ViperWrapper() override = default;

  bool find(const char* key, size_t sz, char* value_out) override {
    std::string value;
    bool found = get_client().get(std::string(key, sz), &value);
    if(found) memcpy(value_out, value.data(), value.size());
    return found;
  }

  bool insert(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    return get_client().put(std::string(key, key_sz), std::string(value, value_sz));
  }

  bool update(const char* key, size_t key_sz, const char* value, size_t value_sz) override {
    return get_client().put(std::string(key, key_sz), std::string(value, value_sz));
  }

  bool remove(const char* key, size_t key_sz) override {
    return get_client().remove(std::string(key, key_sz));
  }

  int scan(const char* key, size_t key_sz, int scan_sz, char*& values_out) override {
    return 0;
  }

  size_t get_size() override { return get_client().get_total_inuse_pmem(); }
};

extern "C" tree_api* create_tree(const tree_options_t& opt) {
  return new ViperWrapper(opt.pool_size);
}