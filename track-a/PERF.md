# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | — | 32.05 | 228 | 0 |
| 1 | Baseline C++ rewrite (mmap I/O, per-thread group vectors) | cf7fdc8 | 0.095 | 101 | 60.818 |
| 2 | Flat latency array CSR arena, lock-free endpoint dict | d4b21ff | 0.110 | 46 | 62.596 |
| 3 | Hardware CRC32 hashing, in-place thread cursors, zero-alloc atomic error tracking, hybrid sort/nth_element, 128KB output streaming buffer | 48a94bb | 0.099 | 24 | 65.683 |
| 4 | Single-pass parallel ingestion (eliminated Pass 2), persistent SimpleBarrier thread pool, immediate madvise(MADV_DONTNEED) page dropping, 390 lines | (evaluating) | ~0.017 | ~19 | ~86+ |
