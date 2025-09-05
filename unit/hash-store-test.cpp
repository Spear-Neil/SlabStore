#include "../store/hash-store.h"

#include <iostream>

using namespace SlabStore;
using namespace util;

int main() {
  {
    HashStore<util::String> store0;
    store0.open("/home/sn/pmem/HashStore0");
    String* key = (String*) malloc(sizeof(String) + 10);
    memcpy(key->str, "hello", 5);
    key->len = 5;
    store0.upsert(*key, nullptr, 0);
    assert(store0.lookup(*key));
    store0.update(*key, nullptr, 0);
    assert(store0.lookup(*key));
    store0.remove(*key);
    assert(!store0.lookup(*key));
  }

/*  {
    HashStore<uint64_t> store1;
    store1.open("/home/sn/pmem/HashStore1");
    store1.upsert(0, nullptr, 0);
    assert(store1.lookup(0));
    store1.update(0, nullptr, 0);
    assert(store1.lookup(0));
    store1.remove(0);
    assert(!store1.lookup(0));
  }*/

  return 0;
}