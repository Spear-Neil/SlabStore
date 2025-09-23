#include<iostream>

#include "cpucounters.h"
#include "util.h"

static size_t mlen = 32 * (0x01ul << 30);

int main(int argc, char* argv[]) {
  pcm::PCM* pcm = pcm::PCM::getInstance();
  pcm->checkError(pcm->program());

  void* mem = malloc(mlen);
  memset(mem, 0, mlen);

  util::Timer timer;
  auto before = pcm->getSystemCounterState();
  timer.start();
  memset(mem, 1, mlen);
  long drt = timer.duration_us();
  auto after = pcm->getSystemCounterState();

  std::cout << "[INFO]: L3 Miss Ratio: " << 1 - pcm::getL3CacheHitRatio(before, after) << std::endl;

  double mem_reads = (double) pcm::getBytesReadFromMC(before, after) / (0x01ul << 20);
  double mem_writes = (double) pcm::getBytesWrittenToMC(before, after) / (0x01ul << 20);
  std::cout << "[INFO]: Mem Reads: " << mem_reads << " MiB, " << mem_reads * 1000000 / drt << " MiB/S" << std::endl;
  std::cout << "[INFO]: Mem Writes: " << mem_writes << " MiB, " << mem_writes * 1000000 / drt << " MiB/S" << std::endl;

  double pmem_reads = (double) pcm::getBytesReadFromPMM(before, after) / (0x01ul << 20);
  double pmem_writes = (double) pcm::getBytesWrittenToPMM(before, after) / (0x01ul << 20);
  std::cout << "[INFO]: PMM Reads: " << pmem_reads << " MiB, " << pmem_reads * 1000000 / drt << " MiB/S" << std::endl;
  std::cout << "[INFO]: PMM Writes: " << pmem_writes << " MiB, " << pmem_writes * 1000000 / drt << " MiB/S"
            << std::endl;

  pcm->cleanup();
  return 0;
}