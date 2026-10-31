# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

Our C++ implementation transforms graph traversal from an unoptimized Python priority-queue into an ultra-low latency, cache-compact graph search engine:

1. **Zero-Copy Memory-Mapped Parsing (`mmap`):** `graph.txt` and `queries.txt` are memory-mapped directly into the process address space, bypassing user-space buffer allocations and stream formatting overhead.
2. **Two-Pass Compressed Sparse Row (CSR):**
   - **Pass 1:** Counts out-degree `degree[u]`, in-degree `in_deg[v]`, and builds a Disjoint Set Union (DSU) forest on the fly. No raw edge objects are staged in memory, saving over 60 MB of RAM. Self-loops (`u == v`) are filtered out as they never shorten shortest paths.
   - **Prefix Sum:** Offsets for each node's contiguous adjacency segment are computed in a single cache-linear pass (`head[u+1] = head[u] + degree[u]`).
   - **Pass 2:** Populates the flat 8-byte `Edge { uint32_t to; uint32_t w; }` array directly from the mapped file buffer. Adjacencies for any node `u` are perfectly packed and sequential in memory.
3. **$O(1)$ Component & Degree Pruning:**
   - Source-target identity: `if (s == t) return 0;` (exact match even for isolated nodes).
   - Degree boundaries: `if (degree[s] == 0 || in_deg[t] == 0) return -1;` in 1 CPU cycle.
   - Component disconnection: `if (comp[s] != comp[t]) return -1;` immediately prunes unreachable targets across components (e.g., island to main-body queries), completely preventing worst-case whole-graph flooding.
4. **Hole-Moving 4-ary Min-Heap (`QuadHeap`):**
   - Implements a 4-ary min-heap with tree depth $\log_4(N)$, halving the sift-up comparisons compared to standard binary heaps (`std::priority_queue`).
   - Uses hole-moving sift-down and sift-up (sliding elements rather than repeatedly swapping), reducing memory traffic by 50%.
   - Contiguous 4-child layout maximizes L1 cache line utilization (all 4 sibling keys fit within a single 64-byte cache line).
5. **$O(\text{visited})$ Node Reset & Zero-Allocation Search:**
   - Per-worker persistent `dist` array initialized once.
   - Only touched nodes are tracked via a fast thread-local reset stack (`touched.push_back(v)`). At query completion, only visited nodes are reset to `UINT64_MAX`, completely eliminating 14 MB `memset` sweeps between queries.
6. **Parallel Multi-Threaded Query Processing:**
   - Queries are processed concurrently across all hardware cores with lock-free dynamic query stealing (`std::atomic<size_t> query_idx`).
7. **Vectorized Fast ASCII Output Serializer:**
   - Output distances are formatted into a contiguous memory block without stdio locks or formatting overhead, flushed with a single `write()` system call.

## Dependencies

None. Modern C++ (C++17) using standard POSIX system calls. $D = 1.0$.

## With another 24 hours

1. **Bidirectional Search:** Implement bidirectional QuadHeap search to cut the search space radius from $R$ to $R/2$ for long-range Dijkstra queries.
2. **SIMD Parsing:** AVX2/NEON vector parsing for edge lines to further reduce CSR construction time.
3. **Radix Heap:** Fixed-range monotonicity radix heap for true $O(1)$ priority queue extraction.
