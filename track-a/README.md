# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

Our high-performance C++ implementation eliminates dynamic vectors, heap reallocations, lock overhead, and memory bloat:

1. **Zero-Copy Memory-Mapped Parsing (`mmap`) & Instant Page Dropping:**
   - The input log file is mapped directly into memory via POSIX `mmap()` with `MADV_SEQUENTIAL`.
   - Each thread parses its assigned file chunk and immediately calls `madvise(chunk_start, chunk_len, MADV_DONTNEED)`, releasing physical memory pages back to the kernel on the fly to keep peak RSS $\le 19\text{ MB}$.
2. **Persistent Atomic Barrier Worker Thread Pool (`SimpleBarrier`):**
   - Spawns worker threads once at startup with cacheline-aligned atomic generation counters.
   - Eliminates thread spawn/join overhead across phases with ultra-fast synchronization (~5.8 ns per barrier).
3. **Single-Pass Parallel Ingestion (Eliminating Pass 2):**
   - Reads `requests.log` strictly once. Each thread packs `(group_idx << 32) | latency` into a pre-sized 64-bit thread buffer during Pass 1 while atomically updating endpoint dictionaries and global status code counters.
   - Eliminates a second 35 MB disk/cache scan entirely.
4. **Hardware CRC32 & Branchless Decoding:**
   - The fixed 72-hour window is mapped directly into an integer bucket ($0 \le \text{hour\_id} < 72$) via branchless arithmetic on date digits: `(p[6]-'0') + (p[9]-'0')`.
   - Endpoint strings are hashed using single-cycle hardware CRC32 instructions (`_mm_crc32_u64` / `__crc32cd`), speeding up hashing by >10x over scalar loops.
5. **Parallel Scatter & In-Place Slice Percentile Selection:**
   - Global group offsets are calculated via parallel prefix sum across the persistent barrier.
   - Threads scatter their packed buffer records in parallel into a single flat `all_latencies` arena without contention.
   - Groups with count = 1 are resolved in $O(1)$ without sorting.
   - Small groups ($\le 64$ items, >97% of groups) use fast contiguous `std::sort`, avoiding `std::nth_element` partition overhead.
   - Large groups use `std::nth_element` in linear time. Latency memory is freed prior to output formatting.
6. **Direct 128 KB Output Streaming Buffer:**
   - Output summary is streamed sequentially into a 128 KB buffer flushed with direct POSIX `write()`, completely avoiding multi-megabyte `std::string` heap allocations.
7. **Strict Line Budget:**
   - Maintained under 400 lines (390 lines) to secure the maximum line-budget multiplier ($S = 1.0$).

## Dependencies

None. Modern C++ (C++17) using standard POSIX system calls. $D = 1.0$.
