/*
 * Copyright (c) 2025-Present, Chen Yuan <yuan.chen@whu.edu.cn>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */

#ifndef SLABSTORE_FS_UTIL_H
#define SLABSTORE_FS_UTIL_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <fcntl.h>

#include "const.h"
#include "util.h"

namespace SlabStore {

/**
 * @brief get total capacity of the underlying storage device
 * */
inline size_t fs_device_capacity(const char* path) {
  struct statvfs st{};
  int res = statvfs(path, &st);
  DEBUG_COND_ERROR(res != 0, "statvfs failed");
  return st.f_frsize * st.f_blocks;
}

/**
 * @brief get available capacity of the underlying storage device
 * */
inline size_t fs_device_available(const char* path) {
  struct statvfs st{};
  int res = statvfs(path, &st);
  DEBUG_COND_ERROR(res != 0, "statvfs failed");
  return st.f_frsize * st.f_bfree;
}

/**
 * @brief check whether the path exists
 * */
inline int fs_path_exist(const char* path) {
  int ret = access(path, F_OK);
  if(ret == 0) return 0; // path exists
  DEBUG_COND_ERROR(errno != ENOENT, "unknown error, access");
  return ret; // No such file or directory
}

/**
 * @brief remove a file or an empty directory
 * */
inline int fs_path_remove(const char* path) {
  int ret = remove(path);
  DEBUG_COND_ERROR(ret == -1, "can't remove %s", path);
  return ret;
}

/**
 * @brief create a directory
 * */
inline int fs_dir_make(const char* path) {
  int ret = mkdir(path, kDirMode);
  DEBUG_COND_ERROR(ret == -1, "failed to mkdir %s", path);
  return ret;
}

/**
 * @brief open or create a file
 * */
inline int fs_file_open(const char* path) {
  int fd = open(path, O_RDWR | O_CREAT, kFileMode);
  DEBUG_COND_ERROR(fd == -1, "failed to open \"%s\"", path);
  return fd;
}

/**
 * @brief close a file
 * */
inline int fs_file_close(int fd) {
  int res = close(fd);
  DEBUG_COND_ERROR(res == -1, "failed to close file");
  return res;
}

/**
 * @brief adjust file size
 * */
inline int fs_file_resize(int fd, size_t len) {
  int ret = ftruncate(fd, len);
  DEBUG_COND_ERROR(ret == -1, "failed to resize file");
  return ret;
}

/**
 * @brief get logical file size
 * */
inline size_t fs_file_length(const char* path) {
  struct stat st{};
  int res = stat(path, &st);
  DEBUG_COND_ERROR(res == -1, "failed get file stat");
  return st.st_size;
}

/**
 * @brief get mmap hint
 * */
inline void* fs_mmap_hint(int fd, size_t len) {
  void* hint = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  DEBUG_COND_ERROR(hint == MAP_FAILED, "failed to mmap");
  int res = munmap(hint, len);
  DEBUG_COND_ERROR(res == -1, "failed to munmap");
  return hint;
}

/**
 * @brief mmap a file to memory
 * */
inline void* fs_file_mmap(int fd, size_t len, void* hint) {
  void* ret = mmap(hint, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  DEBUG_COND_ERROR(ret == MAP_FAILED, "failed to mmap file");
  return ret;
}

/**
 * @brief unmap a file
 * */
inline int fs_file_unmap(void* ptr, size_t len) {
  int res = munmap(ptr, len);
  DEBUG_COND_ERROR(res == -1, "failed to munmap");
  return res;
}

/**
 * @brief pre-allocate physical storage blocks
 * */
inline int fs_space_alloc(int fd, size_t off, size_t len) {
  int ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, off, len);
  DEBUG_COND_ERROR(ret == -1, "failed to allocate disk space");
  return ret;
}

/**
 * @brief reclaim physical storage blocks
 * */
inline int fs_space_reclaim(int fd, size_t off, size_t len) {
  int ret = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, len);
  DEBUG_COND_ERROR(ret == -1, "failed to reclaim disk space");
  return ret;
}

/**
 * @brief get the real occupied physical storage space size of a file
 * */
inline size_t fs_space_usage(const char* path) {
  struct stat st{};
  int res = stat(path, &st);
  DEBUG_COND_ERROR(res == -1, "failed get file stat");
  return st.st_blocks * 512;
}

}

#endif //SLABSTORE_FS_UTIL_H
