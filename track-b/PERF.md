# Track B: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | — | 27.20 | 2,290 | 0 |
| 1 | Baseline C++ rewrite (dense 2D bitsets) | `cf7fdc8` | 5.226 | 2,399 | 19.959 |
| 2 | Sparse CSR postings, 2-pass parallel ingestion, multi-threaded query evaluation | `feff7e1` | 0.974 | 128 | 50.477 |
| 3 | Single-pass fused parallel ingestion & indexing (eliminated serial doc parsing bottleneck), sequential cursor phrase streaming, L1 bloom filter, pre-allocated results & 1MB buffered I/O | pending | ~0.15 | ~45 | ~75+ |
