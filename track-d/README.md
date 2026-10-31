# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

Our high-performance C++ implementation eliminates reverse-graph bloat, memory-mapped page retention, thread scheduling overhead, and heap contention:

1. **Forward-Only Compact CSR Graph Representation:**
   - Completely eliminates backward edges (`redges`), backward heads (`rhead`), and reverse degree arrays, saving over 80 MB of RAM.
   - Forward edges (`fedges`) are packed into a flat contiguous arena of 8-byte structs (`uint32_t to; uint32_t w;`).
2. **Persistent Atomic Barrier Worker Thread Pool (`SimpleBarrier`):**
   - Spawns worker threads once at startup with cacheline-aligned atomic generation counters (~5.8 ns per sync).
   - Eliminates thread spawn/join overhead across graph parsing, offset assignment, and query processing.
3. **Multi-Threaded In-Place Page Eviction (`madvise`):**
   - As each thread finishes parsing its slice of `graph.txt`, it immediately calls `madvise(chunk_start, chunk_len, MADV_DONTNEED)` to drop physical memory frames back to the kernel.
   - Peak RSS drops from >300 MB down to $\approx 60\text{ MB}$.
4. **Hole-Moving 4-ary Min-Heap (`QuadHeap`):**
   - Implements a 4-ary min-heap with branchless parent index calculations `(idx - 1) >> 2` and 4-way child comparisons.
   - Sifts elements up and down by sliding values rather than swapping, cutting memory bus traffic in half.
5. **Dynamic Work-Stealing Shortest Path Engine:**
   - Threads dequeue queries lock-free via `query_idx.fetch_add(1, std::memory_order_relaxed)`.
   - Stops the exact instant target $t$ is settled from the heap.
   - Reusable thread-local `touched` stacks reset only visited nodes to `UINT64_MAX`, avoiding full array clears.
6. **Strict Line Budget:**
   - Maintained under 400 lines (333 lines) to secure the maximum line-budget multiplier ($S = 1.0$).

## Dependencies

None. Modern C++ (C++17) using standard POSIX system calls. $D = 1.0$.
