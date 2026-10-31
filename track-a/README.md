# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

Our high-performance C++ implementation eliminates dynamic vectors, heap reallocations, lock overhead, and memory bloat:

1. **Zero-Copy Memory-Mapped Parsing (`mmap`):** The input log file is mapped directly into memory via POSIX `mmap()` with `MADV_SEQUENTIAL`, avoiding user-space buffer copying.
2. **Hardware CRC32 & Branchless Decoding:**
   - The fixed 72-hour window is mapped directly into an integer bucket ($0 \le \text{hour\_id} < 72$) via branchless arithmetic on date digits: `(p[6]-'0') + (p[9]-'0')`.
   - Endpoint strings are hashed using single-cycle hardware CRC32 instructions (`_mm_crc32_u64` / `__crc32cd`), speeding up hashing by >10x over scalar loops.
3. **Two-Stage Frozen Endpoint Dictionary:**
   - In Pass 1, endpoints are dynamically registered in an open-addressing table with atomic reads.
   - In Pass 2, the dictionary is frozen and queried purely read-only without mutexes or atomic operations.
4. **Memory-Constrained CSR Arena (< 19 MB Peak RSS):**
   - **Pass 1:** Counts records per group and aggregates status codes directly into atomic global counters, completely eliminating thread-local error arrays (saving 7 MB RAM).
   - **In-Place Cursor Conversion:** Thread count arrays are converted in-place into per-thread write cursors, avoiding secondary cursor allocation (saving 7 MB RAM).
   - **Flat Arena:** A single contiguous `uint32_t` array stores all 3.1 million latencies (12.4 MB at full scale).
   - **Pass 2:** Threads stream latencies directly into their assigned slices in parallel. Thread cursor arrays are freed immediately upon pass completion.
5. **Hybrid Percentile Selection:**
   - Groups with count = 1 are resolved in $O(1)$ without sorting.
   - Small groups ($\le 64$ items, >97% of groups) use fast contiguous `std::sort`, avoiding `std::nth_element` partition overhead.
   - Large groups use `std::nth_element` in linear time. Latency memory is freed prior to output formatting.
6. **Direct 128 KB Output Streaming Buffer:**
   - Output summary is streamed sequentially into a 128 KB buffer flushed with direct POSIX `write()`, completely avoiding multi-megabyte `std::string` heap allocations.

## Dependencies

None. Modern C++ (C++17) using standard POSIX system calls. $D = 1.0$.

## With another 24 hours

1. Hand-rolled AVX2 SIMD delimiter scanning for line chunking.
2. Histogram-based percentile estimation for large groups to avoid partitioning altogether.
