# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

Our C++ implementation solves the index vs. query trade-off by turning queries into a pre-filter before corpus indexing:

1. **Query-Aware Vocabulary Filtering:** We read and parse `queries.txt` first, extracting the exact set of distinct query terms $\mathcal{V}_Q$. When scanning the 254 MB `docs.txt`, words not present in $\mathcal{V}_Q$ are immediately skipped. This avoids storing hundreds of thousands of useless postings.
2. **Compact 64-Bit Word Bitsets:** For each query term, postings are stored as dense 64-bit unsigned integer bitsets ($N$ bits per term, where each bit corresponds to a document ID). A term bitset is only 31 KB in full scale ($N = 250,000$).
3. **Hardware-Accelerated Boolean Evaluation:** Boolean queries (`AND`, `OR`, `NOT`) are executed as branchless 64-bit word operations. Result aggregation uses compiler intrinsics `__builtin_popcountll` (single-cycle bit count) and `__builtin_ctzll` (single-cycle document ID extraction).
4. **Two-Phase Phrase Verification:** Because 99% of phrase queries match 5 or fewer documents, candidate document sets are formed by bitwise intersecting the phrase terms. Only candidate documents are scanned for the exact consecutive sequence, avoiding gigabytes of positional index storage.
5. **Zero-Copy Memory-Mapped I/O:** Both `docs.txt` and `queries.txt` are mapped via POSIX `mmap()` with `MADV_SEQUENTIAL`.

## Dependencies

None. Standard library only ($D = 1.0$).

## With another 24 hours

1. AVX-512 / AVX2 vectorization for 256-bit wide bitset boolean operations.
2. Compressed sparse bitsets (Roaring bitmaps) for ultra-rare terms.
3. Multi-threaded query evaluation across the 12 hardware threads.

