# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

Our C++ implementation transforms graph traversal into an ultra-low latency, cache-compact graph search engine:

1. **Zero-Copy Memory-Mapped Parsing (`mmap`):** `graph.txt` and `queries.txt` are memory-mapped directly into process address space.
2. **Parallel Dual Compressed Sparse Row (CSR) Construction:**
   - **Pass 1 (Parallel Degree Counting):** Threads parse chunks of `graph.txt` in parallel to compute both forward out-degrees (`fhead`) and reverse in-degrees (`rhead`).
   - **Prefix Sum:** Direct cache-linear prefix sum transforms degrees into forward and reverse offsets.
   - **Pass 2 (Parallel Dual Edge Placement):** Populates forward edges (`fedges`) and backward edges (`redges`) concurrently. Thread cursor arrays are freed immediately upon completion.
3. **Multi-Threaded Bidirectional Dijkstra Search:**
   - Instead of unidirectional Dijkstra visiting $\pi R^2$ nodes, bidirectional search expands simultaneously from $s$ forward and from $t$ backward, halving the effective search radius and cutting settled node counts by 50–75%.
   - **Early Stopping Bound:** Halts search as soon as $\min(f\_qh) + \min(r\_qh) \ge \text{best\_dist}$.
   - **Step Balancing:** Dynamically steps the smaller frontier (`f_qh.size() <= r_qh.size() ? f_step : r_step`), maintaining balanced ball expansions.
4. **$O(1)$ Boundary & Degree Pruning:**
   - Source-target identity: `if (s == t) return 0;`.
   - Zero out-degree or zero in-degree check: `if (fhead[s+1] == fhead[s] || rhead[t+1] == rhead[t]) return -1;` in 1 CPU cycle.
5. **Hole-Moving 4-ary Min-Heap (`QuadHeap`):**
   - Implements a 4-ary min-heap with tree depth $\log_4(N)$, halving sift-up comparisons compared to binary heaps (`std::priority_queue`).
   - Sift-up and sift-down slide elements rather than swapping, cutting memory traffic in half.
6. **$O(\text{visited})$ Node Reset & Zero-Allocation Search:**
   - Per-worker persistent distance arrays initialized once.
   - Only visited nodes are tracked on a thread-local reset stack (`f_touched`, `r_touched`) and restored to `UINT64_MAX` at query termination, eliminating full-array sweeps.
7. **Vectorized Fast ASCII Output Serializer:**
   - Output distances are formatted into a contiguous memory block without stdio locks, flushed with a single `write()` system call.

## Dependencies

None. Modern C++ (C++17) using standard POSIX system calls. $D = 1.0$.

## With another 24 hours

1. Cache-oblivious Hilbert / BFS node reordering to group adjacent graph vertices in CPU cache lines.
2. SIMD vector parsing for edge lines to further accelerate graph ingestion.
