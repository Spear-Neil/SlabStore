#ifndef SLABSTORE_KV_TYPE_H
#define SLABSTORE_KV_TYPE_H

#include <atomic>
#include <type_traits>
#include "util.h"

namespace util {

struct OptRowValue {}; // optimized kv pair with a version and a valid bit

// Layout of KVPair's first 8-byte (ctrl)
// | invalid bit (1 bit) | tombstone bit (1 bit) | kv version (62 bits) |

template<typename K>
struct KVPair<K, OptRowValue> {
  static constexpr uint64_t kInValid = 0x01ul << 63;
  static constexpr uint64_t kTombstone = 0x01ul << 62;
  static constexpr uint64_t kVerMask = (0x01ul << 62) - 1;
  static constexpr std::memory_order load_order = std::memory_order_acquire;
  static constexpr std::memory_order store_order = std::memory_order_release;

  static_assert(std::is_integral_v<K>); // we only consider integer keys here

  std::atomic<uint64_t> ctrl;
  K key;
  int vlen;
  char value[];

  static void make_kv(KVPair* kv, const K& key, void* value, int vlen) {
    assert(kv != nullptr && (value != nullptr || vlen == 0));
    kv->key = key, kv->vlen = vlen; // tombstone: value = nullptr, vlen = 0
    if(value) memcpy(kv->value, value, vlen);
  }

  void set_control(uint64_t version, bool tombstone = false) {
    if(!tombstone) ctrl.store(version, store_order);
    else ctrl.store(kTombstone | version, store_order);
  }

  void invalidate() { ctrl.store(kInValid, store_order); }

  bool invalid() { return ctrl.load(load_order) & kInValid; }

  bool tombstone() { return ctrl.load(load_order) & kTombstone; }

  uint64_t load_version() { return ctrl.load(load_order) & kVerMask; }
};

template<>
struct KVPair<String, OptRowValue> {
  static constexpr uint64_t kInValid = 0x01ul << 63;
  static constexpr uint64_t kTombstone = 0x01ul << 62;
  static constexpr uint64_t kVerMask = (0x01ul << 62) - 1;
  static constexpr std::memory_order load_order = std::memory_order_acquire;
  static constexpr std::memory_order store_order = std::memory_order_release;

  std::atomic<uint64_t> ctrl;
  int vlen;
  union {
    String key;
    struct {
      int klen;
      char kv[];
    };
  };

  static void make_kv(KVPair* kv, const String& key, void* value, int vlen) {
    assert(kv != nullptr && (value != nullptr || vlen == 0));
    kv->klen = key.len, kv->vlen = vlen; // tombstone: value = nullptr, vlen = 0
    memcpy(kv->key.str, key.str, key.len);
    if(value) memcpy(kv->kv + key.len, value, vlen);
  }

  void set_control(uint64_t version, bool tombstone = false) {
    if(!tombstone) ctrl.store(version, store_order);
    else ctrl.store(kTombstone | version, store_order);
  }

  void invalidate() { ctrl.store(kInValid, store_order); }

  bool invalid() { return ctrl.load(load_order) & kInValid; }

  bool tombstone() { return ctrl.load(load_order) & kTombstone; }

  uint64_t load_version() { return ctrl.load(load_order) & kVerMask; }
};

}

#endif //SLABSTORE_KV_TYPE_H