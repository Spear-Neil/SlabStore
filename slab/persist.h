#ifndef SLABSTORE_PERSIST_H
#define SLABSTORE_PERSIST_H

#include <cstddef>
#include <cstdint>

#include "const.h"
#include "util.h"

namespace SlabStore {

namespace internal {

inline void persist_call(void* ptr, size_t len, void flush_call(void*)) {
  for(size_t uptr = (size_t) ptr & ~(kCacheLineSize - 1);
      uptr < (size_t) ptr + len; uptr += kCacheLineSize) {
    flush_call((void*) uptr);
  }
}

/* write back cache lines to non-volatile/volatile memory,
 * several clwb instructions can be concurrently executed,
 * use it with persist_wait_finish to wait for clwb to complete */
inline void persist_write_back(void* ptr, size_t len) {
  persist_call(ptr, len, util::clwb);
}

/* serially flush cache lines to non-volatile/volatile memory,
 * clflush is identical to one clflushopt plus one mfence/sfence */
inline void persist_serial_flush(void* ptr, size_t len) {
  persist_call(ptr, len, util::clflush);
}

/* concurrently flush cache lines to non-volatile/volatile memory,
 * use it with persist_wait_finish to wait for clflushopt to complete */
inline void persist_concur_flush(void* ptr, size_t len) {
  persist_call(ptr, len, util::clflushopt);
}

inline void persist_wait_finish() { util::mfence(); }

}

using internal::persist_write_back;
using internal::persist_concur_flush;
using internal::persist_wait_finish;

using internal::persist_serial_flush;

/**
 * @brief write back cache lines and wait until completion
 * */
inline void wait_write_back(void* ptr, size_t len) {
  persist_write_back(ptr, len);
  persist_wait_finish();
}

/**
 * @brief concurrently flush cacheline and wait until completion
 * */
inline void wait_concur_flush(void* ptr, size_t len) {
  persist_concur_flush(ptr, len);
  persist_wait_finish();
}

}

#endif //SLABSTORE_PERSIST_H
