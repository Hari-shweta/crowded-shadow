# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | — | 32.05 | 228 | 0 |
| 1 | Baseline C++ rewrite (mmap I/O, per-thread group vectors) | cf7fdc8 | 0.095 | 101 | 60.818 |
| 2 | Zero-vector CSR arena: two-pass parallel aggregation, flat 12.4 MB latency array, lock-free endpoint dictionary, fast digit serializer | (pending next push) | ~0.025 | ~20 | ~80+ |
