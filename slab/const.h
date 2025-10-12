/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_CONST_H
#define SLABSTORE_CONST_H

#include <cstddef>
#include <cstdint>
#include <sys/stat.h>
#include <string>

namespace SlabStore {

// some common used constant, can't be modified
inline constexpr size_t kCacheLineSize = 64;
inline constexpr size_t kPageSize = 0x01ul << 12;  // 4kB
inline constexpr size_t kGigaBytes = 0x01ul << 30; // 1GB

inline constexpr uint32_t kDirMode = S_IRWXU | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
inline constexpr uint32_t kFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;

struct SlabConst {
  /* option of whether to print allocator state/help information */
  static constexpr bool kLogInfo = true;
  /* optimization for platforms that support eADR domain */
  static constexpr bool kEnhancedADR = false;
  /* whether to reclaim physical space */
  static constexpr bool kPhyReclaim = false;
  /* whether to pre-alloc all physical spare during initialization
   * turn off this option for better physical space footprint */
  static constexpr bool kPreAlloc = true;
  /* processor number bound to a single arena */
  static constexpr size_t kCorePerArena = 2;
  /* processor number bound to a RunCase, reduce contention on run allocation */
  static constexpr size_t kCorePerRunCase = kCorePerArena * 2;
  /* the number of persistent root pointers, initialized with nullptr */
  static constexpr size_t kRootCount = 1024;
  /* size of extent, allocation requests get memory blocks from an extent, the start
   * address of extent file except root segment should be aligned on kExtentSize */
  static constexpr size_t kExtentSize = 0x01ul << 22; // 4MB, can't be modified
  /* the size of persistent memory pool must be a multiple of kPoolAlign */
  static constexpr size_t kPoolSizeAlign = 0x01ul << 30;  // 1GB
  /* the base run size for small allocation, can't be modified */
  static constexpr size_t kSmallRunBase = 4 * kPageSize; // 16KB
  /* the base run size for medium allocation, can't be modified */
  static constexpr size_t kMediumRunBase = 32 * kPageSize; // 128KB
  /* the least region size, the least Gap Size (refer to desc.h), can't be modified */
  static constexpr size_t kQuantumSize = 0x01ul << 5; // 32 Byte
  static constexpr size_t kMinGapSize = 0x01ul << 7;  // 128 Byte
  /* the max region size of different allocation category, can't be modified */
  static constexpr size_t kMaxSmallSize = 0x01ul << 11;  // 2KB
  static constexpr size_t kMaxMediumSize = 0x01ul << 16; // 64KB
  static constexpr size_t kMaxLargeSize = 0x01ul << 22;  // 4MB
  /* the max slab size in fast size2index lookup table */
  static constexpr size_t kMaxLookupSize = kPageSize; // 4KB
  /* number of size classes per size group, can't be modified */
  static constexpr size_t kNSlabsPerGrp = 4;
  /* number of size classes in thread cache, can't be modified */
  static constexpr size_t kNSlabsCached = 40;
  /* number of size class groups */
  static constexpr size_t kNSizeGroup = kNSlabsCached / kNSlabsPerGrp;
  /* the capacity of cache bin in each size group */
  static constexpr size_t kCacheBinCapTab[kNSizeGroup] = {128, 128, 64, 64, 32, 16, 16, 8, 8, 4};
  /* stock quantity of cache bin; if the cache bin is full, the regions in excess of stock quantity
   * will be purged into arena; if the cache bin is empty, restock regions from corresponding arena */
  static constexpr size_t kCacheBinStockTab[kNSizeGroup] = {64, 64, 32, 32, 16, 8, 8, 4, 4, 2};
  /* the count of run types (different size), can't be modified */
  static constexpr size_t kRunTypeCount = 6;
  /* run size corresponding to each size group, can't be modified */
  static constexpr size_t kRunSizeTab[kRunTypeCount] = {kSmallRunBase, kSmallRunBase * 2, kSmallRunBase * 4,
                                                        kMediumRunBase, kMediumRunBase * 2, kMediumRunBase * 4};
  /* cached slab size class index to RunBin index map, can't be modified */
  static constexpr size_t kCBin2RBin[kNSizeGroup] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5};

  /* the upper limit number of extents held by each RunBin and LargeBin,
   * redundant free extents will be released to the global ExtentCase */
  static constexpr size_t kExtentHoldCount = 1;
  /* the upper limit number of runs held by ArenaBin, redundant free
   * runs will be released back to the corresponding RunCase */
  static constexpr size_t kRunHoldCount = 1;


  /* meta/extent file name of a nvm pool */
  inline static const std::string kMetaFileName = "meta";
  inline static const std::string kExtentFileName = "extent";

  static_assert(kExtentSize / kSmallRunBase <= 64 * 4);
  static_assert(kExtentSize / kMediumRunBase <= 32);
  static_assert(kNSlabsCached % kNSlabsPerGrp == 0);
  static_assert(kQuantumSize * kNSlabsPerGrp == kMinGapSize);
  static_assert(kMinGapSize << (kNSlabsCached / kNSlabsPerGrp - 1) == kMaxMediumSize);
};

}

#endif //SLABSTORE_CONST_H
