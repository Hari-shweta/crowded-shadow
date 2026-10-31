# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

Our high-performance C++ index and query engine replaces the reference Python implementation with a cache-compact, multi-threaded postings architecture:

1. **Pre-Parsed Query Vocabulary Filtering:**
   Instead of indexing all 609,833 distinct words in the corpus, we parse `queries.txt` first into an open-addressing hash table. Only words that actually appear in queries (75,747 terms) are indexed. 99% of corpus tokens are skipped during document scanning.
2. **Two-Pass Parallel CSR Ingestion (Zero Heap Allocation):**
   - **Pass 1:** Worker threads compute thread-local document frequency counts `local_df[tid]` in flat arrays (300 KB per thread, fitting in L2 cache).
   - **Prefix Sum:** Offsets into a single contiguous `flat_postings` array are computed in $O(\text{vocab})$.
   - **Pass 2:** Worker threads write document IDs directly into their pre-calculated contiguous slots in `flat_postings`. No dynamic vectors, no resizing, zero memory fragmentation.
   - Postings for every term are naturally sorted in strictly ascending order without requiring any sort operations.
3. **Multi-Threaded Query Answering:**
   - Queries are processed concurrently across all CPU threads using dynamic atomic stealing.
   - **BOOL Queries:** Evaluated with thread-local working bitsets (31.25 KB, fitting entirely in L1 cache). Sparse terms ($df < 1000$) use bit-testing in $O(df)$ time (~30 nanoseconds), while dense terms use bitwise logic.
   - **PHRASE Queries:** Term with the minimum document frequency serves as an anchor. Candidate documents are checked against other phrase terms with binary search in sorted posting lists, followed by token sequence verification.
4. **Fast Branchless ASCII Serializer:**
   Answers are formatted into pre-allocated memory buffers without stdio locks or `std::to_string` heap overhead.

## Dependencies

None. Modern C++ (C++17) using standard POSIX system calls. $D = 1.0$.

## With another 24 hours

1. AVX2 SIMD delimiter scanning for the document tokenization loop.
2. 16-bit block partitioning (Roaring bitmap style) to further compress postings to under 40 MB.
