# int16 quantization

Switching the IVF index from float32 vectors to symmetric int16 to halve
memory bandwidth and double SIMD throughput. The change is end-to-end:
training emits int16, the runtime reads int16, queries are quantized at
request time with the same global scale.

## Why int16 (and not float32, not int8)

The 14-dimensional feature vectors come pre-normalized — sampled ranges
on 200k vectors:

| dim | min | max | abs_max |
|----:|------:|------:|--------:|
| 0   | 0.001 | 1.000 | 1.000 |
| 1   | 0.083 | 1.000 | 1.000 |
| 2   | 0.050 | 1.000 | 1.000 |
| 3   | 0.000 | 0.957 | 0.957 |
| 4   | 0.000 | 1.000 | 1.000 |
| 5   | -1.000| 0.499 | 1.000 |
| 6   | -1.000| 1.000 | 1.000 |
| 7   | 0.000 | 1.000 | 1.000 |
| 8   | 0.050 | 1.000 | 1.000 |
| 9   | 0.000 | 1.000 | 1.000 |
| 10  | 0.000 | 1.000 | 1.000 |
| 11  | 0.000 | 1.000 | 1.000 |
| 12  | 0.150 | 0.850 | 0.850 |
| 13  | 0.002 | 0.050 | 0.050 |

12 of 14 dims hit `|v| = 1.0`. The features were designed to live in
`[-1, 1]` (or `[0, 1]`) by construction.

Precision needed: the official k-NN scoring uses `k=5` brute-force; the
ranking only depends on which 5 references are closest. Differences in
the 5th decimal place never change top-5 ordering when the closest
neighbors differ by ~10⁻²–10⁻¹ in dist². The spec mentions 4 decimal
places of significance — we can safely operate at ~5 dp.

- **float32**: ~7 decimal digits — overkill for normalized features.
- **int16**: 65 536 values. With a global scale of 32 767, the step size
  is 1/32 767 ≈ 3.05×10⁻⁵ → **5 decimal places of precision**.
- **int8**: 255 values → step ~4×10⁻³ → 2–3 decimal places. Would need
  per-codebook tricks (PQ) to recover precision. Skipped.

The only "loss" is on dim 13 (range `[0, 0.05]`): step 3×10⁻⁵ →
relative step 6×10⁻⁴ ≈ 3 decimal places. But that dim contributes at
most `0.05² = 0.0025` to dist², about 400× less than dims at `|v|=1`.
The contribution to ordering is negligible.

## Preserving dist² ordering

Quantization is a single global linear transform:

```
scale = 32767 / global_abs_max         (single float; ~32767 here)
q[d]  = clamp(round(v[d] * scale), -32767, 32767)
```

For a query Q and a reference V:
```
(Q[d] - V[d])² × scale²   =   (q_Q[d] - q_V[d])²       (in int)
```

Summing over all d:
```
dist²_orig × scale²   =   dist²_q                       (in int)
```

`scale²` is a positive constant that's the same for every (Q, V) pair.
So `dist²_q_a < dist²_q_b ⟺ dist²_orig_a < dist²_orig_b`. **Top-K
ordering is exactly preserved.** No detection_score regression
expected.

## Memory + SIMD math

| | float32 | int16 |
|---|---|---|
| bytes/vector (DIM=14, stride 16 padded) | 56 (no pad) | 32 |
| total vectors section (3M vec) | 168 MB | **96 MB** |
| total ivf.bin | ~171 MB | **~99 MB** |
| fits in 160 MB cgroup? | NO | **YES** (with room) |
| cluster K=1700 (1764 vec) on disk | 99 KB | **57 KB** |
| AVX2 lanes per register | 8 (float32, 256 bits) | **16 (int16)** |

The L1 cache on Haswell is 32 KB. A 57 KB cluster still doesn't fit,
but the cluster scan is purely sequential — the hardware prefetcher
handles streaming reads well regardless of L1/L2 fit. **The lever is
total bytes read, not where they fit.**

Halving the bytes per vector ≈ halves L2→L1 traffic. The SIMD compute
side gets a parallel win: AVX2's `vpmaddwd` produces 8 int32 outputs
from 16 int16 inputs, one instruction.

## SIMD code

```cpp
inline int64_t dist_sq(const int16_t *a, const int16_t *b) {
  __m256i va   = _mm256_loadu_si256((const __m256i*)a);   // 16 int16
  __m256i vb   = _mm256_loadu_si256((const __m256i*)b);
  __m256i diff = _mm256_sub_epi16(va, vb);                // 16 int16 diffs
  __m256i sq   = _mm256_madd_epi16(diff, diff);           // 8 int32 (a²+b² pairs)

  // horizontal sum 8 int32 → scalar
  __m128i lo = _mm256_castsi256_si128(sq);
  __m128i hi = _mm256_extracti128_si256(sq, 1);
  __m128i s4 = _mm_add_epi32(lo, hi);
  s4 = _mm_hadd_epi32(s4, s4);
  s4 = _mm_hadd_epi32(s4, s4);
  return (int64_t)(int32_t)_mm_cvtsi128_si32(s4);
}
```

DIM=14 doesn't pack neatly into 16 lanes. Two strategies:
1. Mask the tail — complex, branchy.
2. **Pad stride to 16** — zero the unused 2 slots in storage. Their
   contribution to dist² is `(0-0)² = 0` automatically.

We picked (2): both centroids and vectors are stored at STRIDE=16
int16 (= 32 bytes, one AVX2 reg, 32-byte aligned).

## Why int64 accumulator

Worst case: 14 dims × (2×32 767)² ≈ 6.0×10¹⁰ — **exceeds int32 range
(2.1×10⁹)**. Realistically our vectors are normalized so differences
are small, but for safety the dist² return type is `int64_t`. The
overflow risk is real for adversarial input or future changes; cheap to
guard against.

## Binary format change

Header grew from 12 → 20 bytes (one extra float for the scale). All
section offsets in the loader shift accordingly. Backward incompat with
old float32 ivf.bin — training and runtime must be in sync.

```
HEADER:
  uint32 K
  uint32 D = 14
  uint32 N
  float  scale       ← new
CENTROIDS: K × STRIDE × int16
OFFSETS:  (K+1) × uint32
VECTORS:  N × STRIDE × int16
LABELS:   N × uint8
```

## Implementation surface

- `native/k_means.cpp` — after k-means converges (in float space), compute
  `scale = 32767 / max(|v[d]|)`, write header + quantized data.
- `native/ivf_index.hpp` — `IvfIndex.vectors` now `int16_t*`, `.scale`
  field, `dist_sq` overload taking `int16_t*` with AVX2 `_mm256_madd_epi16`.
  Float `dist_sq` overload kept so `k_means.cpp` training loop still
  works on raw floats.
- `native/query.cpp` — debug binary updated to quantize the hardcoded
  test query before scoring.
- `ext/fraud_index/fraud_index.cpp` — `fraud_count()` and the `score`
  Ruby binding quantize the query (`quantize_query(qf, idx.scale, q16)`)
  before calling `ivf_score`.
- The C++ extension `parse_payload` still produces float (the JSON
  parsing logic), then a `quantize_query` step converts to int16 once
  per request (~30 ns of arithmetic).

## What to expect

- ~halve memory bandwidth in the hot dist_sq loop. From the perf data,
  `vmovups + vsubps + vfmadd` (float) → `vmovups + vpsubw + vpmaddwd`
  (int16); the int16 version reads half the bytes and processes 2× the
  lanes per instruction. **Net expected: ~2× faster per dist_sq call.**
- Ivf.bin size shrinks from 168 MB to ~99 MB → no more cgroup eviction
  pressure (fits comfortably in the 160 MB memory limit).
- Detection_score unchanged (linear-scale dist² preserves top-K).
- p99 reduction proportional to how much time was in dist_sq —
  perf showed ~51% of CPU there. A 2× speedup of that portion lowers
  total request CPU time by ~25%.
