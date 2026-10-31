# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | — | 14.44 | 367 | 0 |
| 1 | Initial C++ baseline: scalar parsing, 10MB direct region table, `std::unordered_set<std::string>` distinct names | `f9a22b0` | 2.567 | 249 | 16.775 |
| 2 | SIMD quote-balanced boundary partitioning, multi-threaded record parsing, zero-allocation 64-bit name hashing, in-place L2 sort + parallel k-way merge deduplication | pending board run | ~0.025 | ~8 | ~78-80+ |
