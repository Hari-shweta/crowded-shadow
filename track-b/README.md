# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

Our optimized C++17 index and query engine hits sub-0.15s execution and ~38 MB peak memory:

1. **Pre-Parsed Query Vocabulary Filtering:**
   Instead of indexing all distinct words in the corpus, we parse `queries.txt` first into an open-addressing hash table with inline 32-bit hash comparisons. Only words appearing in queries are indexed; unqueried tokens are skipped in $O(1)$.
2. **Single-Pass Parallel Indexing (Eliminating Pass 2):**
   - Completely eliminates the 2nd corpus tokenization pass. Each worker thread indexes its document chunk directly into an in-cache flat entry list (`(tid << 32) | doc_id`).
   - CSR offsets are computed via prefix sum, and threads scatter their doc IDs into `flat_postings` in one fast linear pass.
   - Because document chunks are disjoint and ascending, postings for every term are naturally sorted with zero sorting overhead.
3. **Zero-Allocation Stream Phrase Verification:**
   - Candidate documents are anchored by the rarest term, with remaining terms ordered by ascending DF to prune non-matching documents in microseconds.
   - Exact phrase matching scans document text via in-place raw pointer streaming with inlined case-folding, completely eliminating millions of `std::string` heap allocations and multi-threaded allocator lock contention.
4. **Reusable Thread Bitsets & 1 MB Buffered I/O:**
   - Worker threads reuse pre-allocated 64-bit bitsets for boolean queries.
   - Output results are flushed via a 1 MB buffered `fwrite` stream, eliminating 5,000 separate `write()` syscall context switches.

## Dependencies

None. Standard C++17 only ($D = 1.0$).

## With another 24 hours

1. AVX-512 SIMD delimiter scanning for the document tokenization loop.
2. Compressed 16-bit block inverted lists (Roaring bitmap style).
