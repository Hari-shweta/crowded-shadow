# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

Our C++ implementation replaces the Python reference's standard library parser and full-sort aggregation with a zero-copy, cache-friendly systems architecture:

1. **Zero-Copy Memory-Mapped I/O:** The input log file is mapped into memory via POSIX `mmap()` with `MADV_SEQUENTIAL`. This bypasses user-space buffer copying and standard library `getline()` lock overhead.
2. **Arithmetic Timestamp Decoding:** Rather than invoking full date/time parsing functions (`strptime`), the fixed 72-hour window is mapped directly into an integer bucket ($0 \le \text{hour\_id} < 72$) via direct arithmetic on the day and hour digits.
3. **Flat Open-Addressing Hash Table:** Endpoints are catalogued using a fixed-capacity linear-probing hash table (FNV-1a hash) storing string views that point directly into the mmapped buffer without allocating `std::string` objects.
4. **Multi-Threaded Execution:** The file is partitioned into newline-aligned byte chunks across hardware threads (up to 12 threads on the 6 P-cores). Each thread aggregates into thread-isolated group structures to completely prevent cache false sharing.
5. **Linear-Time Percentile Selection:** To compute nearest-rank percentiles (`p50`, `p95`, `p99`), we utilize `std::nth_element` (Quickselect/Introselect), finding the exact percentiles in $O(N)$ linear time without fully sorting latency vectors.
6. **Zero-Overhead Output Sorting:** By sorting the small dictionary of distinct endpoints ($\le 1,842$) once, output lines are emitted in chronologically and lexicographically sorted order naturally, eliminating the need to sort the tens of thousands of output rows.

## Dependencies

None. Written in modern C++ (C++17) using standard POSIX system calls. $D = 1.0$.

## With another 24 hours

1. Hand-rolled SIMD (AVX2) for space and newline delimiter scanning to further accelerate line chunking.
2. Direct branchless custom integer-to-string serializer (e.g. fast itoa / Ryu algorithm) for the output formatting phase.
3. Radix-histogram percentile estimation for large groups to avoid partitioning altogether.

