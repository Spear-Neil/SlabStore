# SlabStore: A Write-Optimized and Space-Efficient Key-Value Store for Persistent Memory
SlabStore is a DRAM-PMem hybrid key-value store based on a tailored slab PMem allocator. The index structure resides in
volatile memory, and it maintains only records on PMem. For space-efficiency, it employs a slab allocation strategy like
jemalloc and maintains selective persistence merely for allocator metadata. Given that random small writes on PMem induced 
by frequent allocation/de-allocation are highly expensive (a single bit flipping in the bitmap triggers a 256-byte PMem 
media write), we also propose a write-coalescing technique that enables log-free reuse of garbage records/objects without
additional write overhead. This is enabled by integrating a valid bit flag within the record layout that coordinates with
both the PMem allocator and MVCC versioning system. We don't releasing garbage objects to the PMem allocator; instead,
we cache them in thread-local structures for subsequent log-free reuse, which thus eliminates reclamation write cost. These
garbage records are then reused for incoming updates by using the valid bit to enforce failure atomicity/crash consistency. 
As the valid bit write and record content writes are spatially local, writes will be combined in hardware buffers and then
written into PMem media, which thus eliminates write amplification induced by metadata modification during allocation. In
the end, SlabStore is space-efficient and has comparable or higher performance than existing PMem-optimized key-value stores.

The disadvantage of SlabStore is that it necessitates a full scan of allocated PMem blocks to reconstruct in-memory 
structures including index structures and allocator's auxiliary data structures, if a power failure or system crash occurs.
However, PMem's read bandwidth is only half lower than that of DRAM, enabling a full device scan in tens of seconds. Hence,
a well-programmed and parallel recovery routine can accomplish in tens of seconds with modern multi-core processors. And
meanwhile, system crash and power failure are rare events in production systems. Pursuing whole system's instant recovery 
while inevitably incurring expensive operational write overhead is inefficient and uneconomic. SlabStore thus maintains 
minimal crash consistency guarantees for failure recovery and employs a full scan reconstruction after failures. By the way,
it also provides a fast reboot mode for a clean/normal shutdown.


# Requirements
* x86-64 CPU supporting persistent memory and clwb, mfence, SSE2 instructions
* Intel Threading Building Blocks (TBB) (`apt install libtbb-dev`)
* jemalloc (`apt install libjemalloc-dev`)
* C++17 compliant compiler
* [pmdk](https://github.com/pmem/pmdk.git) and [pmemkv](https://github.com/pmem/pmemkv.git) (`apt install libpmemkv-dev`)

# Project Structure
Directories `slab` and `store` include code for our tailored PMem slab allocator and SlabStore, respectively. `scripts`
contains python scripts for experiments and figures. The experiment related code is in the `test` directory. The `unit`
directory contains some code for unit test.

# Get Started
1. Clone this repository and initialize the submodules
```
git clone <repository name>
cd <project directory>
git submodule init && git submodule update

cd <project directory>/test/pibench/
git submodule init && git submodule update
```
2. Back to the project directory and create a new directory *build* `mkdir build && cd build`
3. Build the project `cmake -DCMAKE_BUILD_TYPE=Release .. && make -j`
4. Run the example `SlabStore-Example /mnt/pmem0/slabstore`