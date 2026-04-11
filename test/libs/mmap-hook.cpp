#include <iostream>
#include <atomic>
#include <cstring>
#include <cstdarg>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <dlfcn.h>
#include <numa.h>
#include <numaif.h>

enum STORE_TYPE {
  PMEMKV = 0, BASIC_SLABKV, SLABKV, PLUSHKV, VIPERKV, ROCKSKV,
  FIXED_VIPERKV, DUMMYKV, NUM_KVSTORE
};

typedef void* (*mmap_func)(void*, size_t, int, int, int, off_t);

typedef int (*munmap_func)(void*, size_t);

static mmap_func real_mmap = nullptr;
static munmap_func real_munmap = nullptr;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void write_unsigned(int fd, uint64_t n) {
  char buf[24];
  int i = 0;
  do {
    buf[i++] = '0' + (n % 10);
    n /= 10;
  } while(n);
  while(i--) write(fd, &buf[i], 1);
}

static void write_signed(int fd, int64_t n) {
  if(n < 0) {
    write(fd, "-", 1);
    n = -n;
  }
  write_unsigned(fd, (uint64_t) n);
}

static void write_string(int fd, const char* s) {
  if(s == nullptr) s = "(null)";
  write(fd, s, strlen(s));
}

static void tiny_fprintf(int fd, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  for(const char* p = fmt; *p; p++) {
    if(*p != '%') {
      write(fd, p, 1);
      continue;
    }
    p++;  // skip '%'
    if(*p == 'd') {
      write_signed(fd, va_arg(ap, int64_t));
    } else if(*p == 'z' && *(p + 1) == 'u') {
      p++;  // skip 'z'
      write_unsigned(fd, va_arg(ap, uint64_t));
    } else if(*p == 's') {
      write_string(fd, va_arg(ap, const char*));
    } else {
      write(fd, "%", 1);
      write(fd, p, 1);
    }
  }
  va_end(ap);
}

static void safe_get_path(int fd, char* buf, size_t size) {
  if(fd < 0) {
    strcpy(buf, "(invalid fd)");
    return;
  }

  char link_path[64];
  char* p = link_path;
  const char* prefix = "/proc/self/fd/";
  while(*prefix) *p++ = *prefix++;
  if(fd == 0) { *p++ = '0'; } else {
    char tmp[16];
    int i = 0;
    int n = fd;
    while(n) {
      tmp[i++] = '0' + (n % 10);
      n /= 10;
    }
    while(i--) *p++ = tmp[i];
  }
  *p = '\0';

  ssize_t len = readlink(link_path, buf, size);
  if(len != -1) {
    buf[len] = '\0';
  } else {
    strcpy(buf, "[unknown]");
  }
}

static STORE_TYPE store_type;

static void safe_get_cmdline(char* buf, size_t size) {
  int fd = open("/proc/self/cmdline", O_RDONLY);
  if(fd < 0) {
    buf[0] = '\0';
    return;
  }

  ssize_t count = read(fd, buf, size - 1);
  if(count < 0) count = 0;
  buf[count] = '\0';

  int arg3_pos = 0, argc = 0;
  for(size_t i = 0; i < count; i++) {
    if(buf[i] == '\0') {
      argc++;
      if(argc == 3) {
        arg3_pos = i + 1;
        break;
      }
    }
  }
  if(buf[arg3_pos + 1] != '\0') {
    exit(-1);
  }

  store_type = (STORE_TYPE) (buf[arg3_pos] - '0');

  for(size_t i = 0; i < count; i++) {
    if(buf[i] == '\0') buf[i] = ' ';
  }
}

static char cmdline_buf[1024 * 8];

static void hook_init() {
  real_mmap = (mmap_func) dlsym(RTLD_NEXT, "mmap");
  if(real_mmap == nullptr) { exit(-1); }
  real_munmap = (munmap_func) dlsym(RTLD_NEXT, "munmap");
  if(real_munmap == nullptr) { exit(-1); }
  safe_get_cmdline(cmdline_buf, sizeof(cmdline_buf));
  tiny_fprintf(STDOUT_FILENO, "mmap hook initialized for process:\n   %s\n", cmdline_buf);
  tiny_fprintf(STDOUT_FILENO, "store type: %d\n", store_type);
}

static std::atomic<void*> pmdk_heap = nullptr;

static void* pmemkv_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  if(fd != -1) {
    tiny_fprintf(STDOUT_FILENO, "mmap called: addr=%zu, length=%d, prot=%d, flags=%d, fd=%d, offset=%d\n",
                 addr, length, prot, flags, fd, offset);
    char path_buf[256];
    safe_get_path(fd, path_buf, sizeof(path_buf));
    tiny_fprintf(STDOUT_FILENO, "  fd %d points to: %s\n", fd, path_buf);
    // void* heap = real_mmap(addr, length, prot, flags, fd, offset);
    // tiny_fprintf(STDOUT_FILENO, "  mmap returned: %zu\n", heap);
    // return heap;
    if(pmdk_heap == nullptr) {
      // it seems that pmemkv mmap/munmap the first page again, here we directly return the same page for that page
      // use the above code snippet to track this behaviour
      pmdk_heap = real_mmap(addr, length, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
      if(pmdk_heap != MAP_FAILED) {
        if((uintptr_t) pmdk_heap.load() % 4096 != 0) {
          tiny_fprintf(STDOUT_FILENO, "Warning: address not page-aligned\n");
        }
        if(length % 4096 != 0) {
          tiny_fprintf(STDOUT_FILENO, "Warning: length not multiple of page size\n");
        }
        memset(pmdk_heap, 0, 4096);
        if(numa_max_node() < 1) {
          tiny_fprintf(STDERR_FILENO, "System has no node 1, max_node=%d\n", numa_max_node());
          return pmdk_heap;
        }
        unsigned long nodemask = 0;
        nodemask |= (0x01ul << numa_max_node()); // node 1
        // there is little bug in mbind, see https://lists.openwall.net/linux-kernel/2010/07/26/113
        long res = mbind(pmdk_heap, length, MPOL_BIND, &nodemask,
                         numa_max_node() + 2, MPOL_MF_MOVE);
        if(res < 0) {
          tiny_fprintf(STDOUT_FILENO, "mbind failed with error: %s\n", strerror(errno));
        }
      }
    }
    return pmdk_heap;
  }

  return real_mmap(addr, length, prot, flags, fd, offset);
}

static void* slabstore_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  static bool mmap_hint = true;
  if(fd != -1) {
    tiny_fprintf(STDOUT_FILENO, "mmap called: addr=%zu, length=%d, prot=%d, flags=%d, fd=%d, offset=%d\n",
                 addr, length, prot, flags, fd, offset);
    char path_buf[256];
    safe_get_path(fd, path_buf, sizeof(path_buf));
    tiny_fprintf(STDOUT_FILENO, "  fd %d points to: %s\n", fd, path_buf);

    flags = MAP_ANONYMOUS | MAP_PRIVATE;
    if(addr != nullptr) flags |= MAP_FIXED;
    void* heap = real_mmap(addr, length, PROT_READ | PROT_WRITE, flags, -1, 0);
    if(heap != MAP_FAILED) {
      if(mmap_hint) {
        mmap_hint = false;
        return heap;
      }

      if((uintptr_t) heap % 4096 != 0) {
        tiny_fprintf(STDOUT_FILENO, "Warning: address not page-aligned\n");
      }
      if(length % 4096 != 0) {
        tiny_fprintf(STDOUT_FILENO, "Warning: length not multiple of page size\n");
      }
      memset(heap, 0, 4096);
      if(numa_max_node() < 1) {
        tiny_fprintf(STDERR_FILENO, "System has no node 1, max_node=%d\n", numa_max_node());
        return heap;
      }
      unsigned long nodemask = 0;
      nodemask |= (0x01ul << numa_max_node()); // node 1
      // there is little bug in mbind, see https://lists.openwall.net/linux-kernel/2010/07/26/113
      long res = mbind(heap, length, MPOL_BIND, &nodemask,
                       numa_max_node() + 2, MPOL_MF_MOVE);
      if(res < 0) {
        tiny_fprintf(STDOUT_FILENO, "mbind failed with error: %s\n", strerror(errno));
      }
    }
  }
  return real_mmap(addr, length, prot, flags, fd, offset);
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  pthread_once(&init_once, hook_init);
  switch(store_type) {
    case PMEMKV:
      return pmemkv_mmap(addr, length, prot, flags, fd, offset);
    case BASIC_SLABKV:
    case SLABKV:
      return slabstore_mmap(addr, length, prot, flags, fd, offset);
    default:
      return real_mmap(addr, length, prot, flags, fd, offset);
  }
}

static int pmemkv_munmap(void* addr, size_t length) {
  if(pmdk_heap.load() == addr) {
    return 0; // it seems that pmemkv mmap/munmap the first page again, (perhaps to read root pointer?)
    // here we directly return success for that page, to avoid double unmapping and potential crash
  }
  return real_munmap(addr, length);
}

static int slabstore_munmap(void* addr, size_t length) {
  return real_munmap(addr, length);
}

int munmap(void* addr, size_t length) {
  pthread_once(&init_once, hook_init);
  // tiny_fprintf(STDOUT_FILENO, "munmap called: addr=%zu, length=%d\n", addr, length);

  switch(store_type) {
    case PMEMKV:
      return pmemkv_munmap(addr, length);
    case BASIC_SLABKV:
    case SLABKV:
      return slabstore_munmap(addr, length);
    default:
      return real_munmap(addr, length);
  }
}