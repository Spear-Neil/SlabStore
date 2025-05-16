#include <iostream>
#include "../slab/fs-util.h"

using namespace SlabStore;

int main() {
  std::cout << "[INFO]: fs-util test" << std::endl;
  const char* path = "/tmp/slab-pool";
  int test_cnt = 0;
  size_t capacity = fs_device_capacity("/tmp");
  printf("[INFO]: device capacity %zu Bytes, %f Gigabytes\n", capacity, double(capacity) / kGigaBytes);
  size_t avail = fs_device_available("/tmp");
  printf("[INFO]: device available %zu Bytes, %f Gigabytes\n", avail, double(avail) / kGigaBytes);
  int res = fs_path_exist(path);
  printf("[INFO]: path %s\n", (res == 0) ? "exist" : "does not exist");
  res = fs_dir_make(path);
  printf("test %i: dir make, %s\n", test_cnt++, (res == -1) ? "failed" : "pass");
  res = fs_path_remove(path);
  printf("test %i: dir remove, %s\n", test_cnt++, (res == -1) ? "failed" : "pass");

  int fd = fs_file_open(path);
  printf("test %i: open file, %s\n", test_cnt++, (fd == -1) ? "failed" : "pass");
  printf("[INFO]: file length: %zu, physical space usage: %zu\n", fs_file_length(path), fs_space_usage(path));
  res = fs_file_resize(fd, kPageSize);
  printf("test %i: resize file, %s\n", test_cnt++, (res == -1) ? "failed" : "pass");
  printf("[INFO]: file length: %zu, physical space usage: %zu\n", fs_file_length(path), fs_space_usage(path));
  res = fs_space_alloc(fd, 0, kPageSize);
  printf("test %i: space allocation, %s\n", test_cnt++, (res == -1) ? "failed" : "pass");
  printf("[INFO]: file length: %zu, physical space usage: %zu\n", fs_file_length(path), fs_space_usage(path));
  res = fs_space_reclaim(fd, 0, kPageSize);
  printf("test %i: space reclamation, %s\n", test_cnt++, (res == -1) ? "failed" : "pass");
  printf("[INFO]: file length: %zu, physical space usage: %zu\n", fs_file_length(path), fs_space_usage(path));
  res = fs_space_reclaim(fd, 0, kPageSize);
  printf("test %i: space re-reclamation, %s\n", test_cnt++, (res == -1) ? "failed" : "pass");
  void* hint = fs_mmap_hint(fd, kPageSize);
  printf("test %i: mmap hint, %s\n", test_cnt++, (hint == MAP_FAILED) ? "failed" : "pass");
  void* addr = fs_file_mmap(fd, kPageSize, hint);
  printf("test %i: mmap file, %s\n", test_cnt++, (addr == MAP_FAILED || addr != hint) ? "failed" : "pass");
  res = fs_file_unmap(addr, kPageSize);
  printf("test %i: munmap file, %s\n", test_cnt++, (res == -1) ? "failed" : "pass");
  res = fs_file_close(fd);
  printf("test %i: close file, %s\n", test_cnt++, (res == -1) ? "failed" : "pass");
  fs_path_remove(path);

  return 0;
}