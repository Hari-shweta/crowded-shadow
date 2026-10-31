# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

Our high-performance C++ implementation eliminates all dynamic vectors, heap reallocations, and lock overhead:

1. **Zero-Copy Memory-Mapped Parsing (`mmap`):** The input log file is mapped directly into memory via POSIX `mmap()` with `MADV_SEQUENTIAL`, avoiding user-space buffer copying.
2. **Arithmetic Timestamp Decoding:** The fixed 72-hour window is mapped directly into an integer bucket ($0 \le \text{hour\_id} < 72$) via direct arithmetic on the day and hour digits, bypassing calendar parsing.
3. **Flat Open-Addressing Hash Table:** Endpoints are catalogued using a fixed-capacity linear-probing hash table (FNV-1a hash) with lock-free atomic reads.
4. **Two-Pass Parallel CSR Ingestion (Zero Dynamic Heap Allocations):**
   - **Pass 1:** Threads count occurrences per `(hour, endpoint)` group in flat 576 KB arrays fitting in CPU L2 cache. Prefix sums determine the exact group offsets.
   - **Flat Arena:** A single contiguous `uint32_t` array holds all 3.1 million latencies (exactly 12.4 MB at full scale).
   - **Pass 2:** Threads write latencies directly into their pre-assigned slices. Zero dynamic `std::vector` instances are allocated across the entire run.
5. **Linear-Time Percentile Selection:** To compute nearest-rank percentiles (`p50`, `p95`, `p99`), `std::nth_element` is executed directly on contiguous group slices in $O(N)$ linear time.
6. **Fast Branchless Digit Serializer:** Group summaries are serialized into pre-allocated memory buffers without stdio locks or formatting overhead.

## Dependencies

None. Modern C++ (C++17) using standard POSIX system calls. $D = 1.0$.

## With another 24 hours

1. Hand-rolled AVX2 SIMD delimiter scanning for line chunking.
2. Histogram-based percentile estimation for large groups to avoid partitioning altogether.
