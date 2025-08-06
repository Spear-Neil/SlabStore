/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_KV_TYPE_H
#define SLABSTORE_KV_TYPE_H

#include <atomic>
#include "util.h"

namespace util {

struct OptRowValue {}; // optimized kv pair with a version and a valid bit

template<typename K>
struct KVPair<K, OptRowValue> {
  std::atomic<uint64_t> ctrl;
  K key;
  int vlen;
  char value[];
};

template<>
struct KVPair<String, OptRowValue> {
  std::atomic<uint64_t> ctrl;
  int vlen;
  union {
    String key;
    struct {
      int klen;
      char kv[];
    };
  };
};

}

#endif //SLABSTORE_KV_TYPE_H