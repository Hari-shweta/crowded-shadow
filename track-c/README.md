# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

Our C++ implementation tackles the edge cases of the hostile CSV specification while delivering maximum parsing speed:

1. **Mid-File Byte Order Mark Handling:** We check for and strip the 3-byte UTF-8 BOM (`0xEF 0xBB 0xBF` / `U+FEFF`) both at the beginning of the file and at chunk boundaries directly after record terminators.
2. **Robust Multi-Line CSV Parsing:** The parser correctly preserves quoted commas, escaped quotes (`""` $\to$ `"`), embedded line feeds (`\n`), and mixed record endings (`\r\n` vs `\n`).
3. **Single-Cycle O(1) Region Lookup:** Rather than maintaining a hash map for 200,000 regions, we construct a 10 MB direct-indexed lookup table (`region_id < 10,000,000`), turning the relational join into a single L3-cache-friendly array lookup.
4. **Online Streaming Aggregates (Zero Memory Waste):** Neither note texts nor full record rows are stored in memory. Unescaped note lengths, name lengths, and integer cents are accumulated on-the-fly.
5. **Exact Integer Cents Arithmetic:** Money is parsed directly into 64-bit integer cents, eliminating all floating-point rounding errors.
6. **Zero-Copy Memory-Mapped I/O:** Both `records.csv` and `regions.csv` are mapped into virtual address space via POSIX `mmap()` with sequential read advice.

## Dependencies

None. Standard library only ($D = 1.0$).

## With another 24 hours

1. SIMD-accelerated quote and delimiter scanning (AVX2).
2. Parallel chunk parsing across threads using quote-balanced boundary detection.
3. Custom flat string-interning arena for distinct name tracking.

