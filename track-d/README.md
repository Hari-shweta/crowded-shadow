# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

Our C++ implementation transforms graph traversal into an ultra-low latency, cache-compact graph search engine:

1. **Zero-Copy Memory-Mapped Parsing (`mmap`):** `graph.txt` and `queries.txt` are memory-mapped directly into process address space.
2. **Two-Pass Compressed Sparse Row (CSR) with Zero Redundant Arrays:**
   - **Pass 1:** Counts out-degree directly in `head[u+1]` (eliminating a 7 MB `degree` array), records in-degrees in a compact 212 KB bitset `has_in_edge` (eliminating a 7 MB `in_deg` array), and builds a Disjoint Set Union (DSU) forest on the fly.
   - **Prefix Sum:** Prefix sum on `head` converts degrees into offsets in a single cache-linear pass.
   - **Immediate Memory Reclamation:** `dsu.parent` is deallocated immediately after computing `comp[n]`.
   - **Pass 2:** Populates the flat 8-byte `Edge { uint32_t to; uint32_t w; }` array directly from the mapped file buffer. `cur` write cursors are deallocated immediately after.
3. **$O(1)$ Component & Degree Pruning:**
   - Source-target identity: `if (s == t) return 0;` (exact match even for isolated nodes).
   - Degree boundaries: `if (head[s + 1] == head[s] || !has_in_edge(t)) return -1;` in 1 CPU cycle with zero auxiliary array overhead.
   - Component disconnection: `if (comp[s] != comp[t]) return -1;` immediately prunes unreachable targets across components (e.g., island to main-body queries), completely preventing worst-case whole-graph flooding.
4. **Hole-Moving 4-ary Min-Heap (`QuadHeap`):**
   - Implements a 4-ary min-heap with tree depth $\log_4(N)$, halving sift-up comparisons compared to standard binary heaps (`std::priority_queue`).
   - Uses hole-moving sift-down and sift-up (sliding elements rather than repeatedly swapping), reducing memory traffic by 50%.
   - Contiguous 4-child layout maximizes L1 cache line utilization (all 4 sibling keys fit within a single 64-byte cache line).
5. **$O(\text{visited})$ Node Reset & Zero-Allocation Search:**
   - Per-worker persistent `dist` array initialized once.
   - Only touched nodes are tracked via a fast thread-local reset stack (`touched.push_back(v)`). At query completion, only visited nodes are reset to `UINT64_MAX`, completely eliminating 14 MB `memset` sweeps between queries.
6. **Parallel Multi-Threaded Query Processing:**
   - Queries are processed concurrently across CPU cores with lock-free dynamic query stealing (`std::atomic<size_t> query_idx`).
7. **Vectorized Fast ASCII Output Serializer:**
   - Output distances are formatted into a contiguous memory block without stdio locks or formatting overhead, flushed with a single `write()` system call.

## Dependencies

None. Modern C++ (C++17) using standard POSIX system calls. $D = 1.0$.

## With another 24 hours

1. Bidirectional search across reverse CSR to cut large-rank query search radius by 50%.
2. SIMD (AVX2/NEON) vector parsing for edge lines to further reduce CSR construction time.
