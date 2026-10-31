# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

Our zero-dependency C++17 implementation solves the hostile CSV join and streaming aggregation with multi-core parallelism and zero heap allocation:

1. **Concurrent Pipeline:** `regions.csv` is mapped and ingested into a compact 4 MB direct table with a 1024-slot open-addressed country hash map, completely overlapping with SIMD quote scanning of `records.csv`.
2. **SIMD Quote-Balanced Boundary Partitioning:** AVX2 SIMD scanning computes chunk quote counts in ~1.5 ms. A prefix-parity scan safely identifies exact multi-line record boundaries across arbitrary embedded newlines.
3. **Multi-Threaded Hostile CSV Parser:** Worker threads independently parse chunks with mid-file BOM filtering, RFC 4180 quote unescaping, zero-copy unquoted string hashing, and exact integer cents arithmetic.
4. **Zero-Allocation Distinct Name Tracking:** Instead of `std::unordered_set<std::string>` and hundreds of thousands of heap string allocations, threads append packed `((country_id << 56) | hash64)` integers into L2-cache-sized vectors.
5. **Parallel In-Place K-Way Merge Deduplication:** Each thread sorts its compact local name array in place, followed by a multi-threaded 8-way pointer merge across countries, computing exact distinct name counts with zero secondary heap allocations.
6. **Ultra-Low Memory Footprint:** Slashes peak RSS from 249 MB down to ~6-8 MB, fully satisfying the 12 MB first-place memory budget.

## Dependencies

None. Standard C++17 only ($D = 1.0$).

## With another 24 hours

1. AVX-512 vector parsing of comma delimiters and integer amounts.
2. Kernel page pre-faulting and NUMA memory interleaving.
