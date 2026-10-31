# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | — | 48.0 | 915 | 0 |
| 1 | Baseline C++ rewrite (unidirectional QuadHeap) | cf7fdc8 | 1.688 | 383 | 38.185 |
| 2 | Memory-lean CSR: 212 KB bitset for in-degrees, head degree extraction, immediate cursor reclamation | d4b21ff | 1.503 | 369 | 39.550 |
| 3 | Bidirectional Dijkstra with 212KB L2 bitset pruning | feff7e1 | 0.566 | 305 | 50.136 |
| 4 | Forward-only compact CSR, persistent SimpleBarrier thread pool, immediate madvise(MADV_DONTNEED) page dropping, dynamic work-stealing QuadHeap, 333 lines | (evaluating) | ~0.12 | ~60 | ~72+ |
