# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | — | 48.0 | 915 | 0 |
| 1 | Baseline C++ rewrite (unidirectional QuadHeap) | cf7fdc8 | 1.688 | 383 | 38.185 |
| 2 | Memory-lean CSR: 212 KB bitset for in-degrees, head degree extraction, immediate DSU and cursor reclamation, fast ASCII serializer | (pending next push) | ~1.25 | ~150 | ~55+ |
