#include <iostream>
#include <thread>
#include "util.h"


using namespace util;

template<typename K, typename V>
class Demo {
  typedef util::KVPair<K, V> KVPair;

  KVPair* kv_;

 private:
  uint64_t hash_code_impl(const K& key, std::true_type) {
    return hash(kv_->key.str, kv_->key.len);
  }

  uint64_t hash_code_impl(const K& key, std::false_type) {
    return hash(kv_->key);
  }

 public:
  Demo(KVPair* kv) : kv_(kv) {}


  uint64_t hash_code() {
    if constexpr(std::is_same<K, String>()) {
      return hash(kv_->key.str, kv_->key.len);
    } else {
      return hash(kv_->key);
    }
//    return hash_code_impl(kv_->key, std::is_same<K, String>());
  }
};


int main() {
  KVPair<uint64_t, uint64_t> kv{.key=1, .value=2};
  Demo<uint64_t, uint64_t> demo(&kv);

  std::cout << demo.hash_code() << std::endl;

  char str[] = "neil";
  KVPair<String, uint64_t>* skv = KVPair<String, uint64_t>::make_kv(str, 4, 2);
  Demo<String, uint64_t> sdemo(skv);
  std::cout << sdemo.hash_code() << std::endl;

  return 0;
}