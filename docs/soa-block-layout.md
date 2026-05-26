# SoA-block layout (the optimization we haven't done yet)

A way to rearrange the IVF vector storage so the AVX2 inner loop processes
8 vectors in parallel instead of 1, drops horizontal reductions, and
enables an "early-exit after 8 of 14 dimensions" trick. The Rust handler
in [jairoblatt/rinha-2026-node](https://github.com/jairoblatt/rinha-2026-node)
uses exactly this layout — it's the main reason their p99 is in
single-digit ms with room to spare.

This doc explains what the layout is, why it wins, and what it would
take to implement.

## Current layout (AoS)

In `native/ivf.bin`, each vector sits contiguously, with 2 zero-padded
slots at the tail to round the 14-dim vector up to 16 int16 (= 32 bytes,
one AVX2 register width):

```
vector 0:  [d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 d10 d11 d12 d13 0  0]   ← 32 bytes
vector 1:  [d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 d10 d11 d12 d13 0  0]   ← 32 bytes
vector 2:  [d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 d10 d11 d12 d13 0  0]   ← 32 bytes
...
```

This is **Array of Structures**: each "structure" is one vector with its
14 dimensions laid out together.

The hot loop in `ivf_index.hpp::dist_sq` for one vector is:

```cpp
__m256i va   = _mm256_loadu_si256(a);              // load 16 int16 of vector
__m256i vb   = _mm256_loadu_si256(b);              // load 16 int16 of query
__m256i diff = _mm256_sub_epi16(va, vb);           // 16 differences
__m256i sq   = _mm256_madd_epi16(diff, diff);      // 8 int32 = pairs of d²+d²

// horizontal reduce 8 int32 → 1 scalar (the dist²):
__m256i lo64 = _mm256_cvtepi32_epi64(...lower...);
__m256i hi64 = _mm256_cvtepi32_epi64(...upper...);
__m256i sum  = _mm256_add_epi64(lo64, hi64);
__m128i s2   = _mm_add_epi64(low(sum), high(sum));
int64_t result = extract(s2, 0) + extract(s2, 1);

// then compare result to top_d[K-1] (scalar), maybe insert into top-K
```

Per vector: ~8 instructions, half of them just to fold the 8 lanes back
into one scalar. The horizontal reduce is **pure overhead** — it adds no
information, it just collapses an intermediate state. AVX2 is bad at
horizontal reductions (no native instruction; sequence of permute + add).

Plus, 12.5% of every load is wasted on the two padding zeros.

## Proposed SoA-block layout

Instead of "one vector at a time", group 8 vectors into a **panel** and
store them dimension-major:

```
panel 0 (8 vectors = v0..v7):
  [v0d0 v1d0 v2d0 v3d0 v4d0 v5d0 v6d0 v7d0]    ← 16 bytes, dim 0 of all 8
  [v0d1 v1d1 v2d1 v3d1 v4d1 v5d1 v6d1 v7d1]    ← 16 bytes, dim 1 of all 8
  [v0d2 v1d2 v2d2 v3d2 v4d2 v5d2 v6d2 v7d2]
  ...
  [v0d13 v1d13 ... v7d13]                       ← 16 bytes, dim 13 of all 8

Total per panel: 14 × 16 bytes = 224 bytes (no padding).
```

Eight vectors are packed into the panel, but their dimensions are
**interleaved across the panel** rather than each vector being
contiguous. Concretely:

```
byte offset:  0    2    4    6    8   10   12   14   16   18   20  ...
content:     v0d0 v1d0 v2d0 v3d0 v4d0 v5d0 v6d0 v7d0 v0d1 v1d1 v2d1 ...
             ─────── 8 int16 of dim 0 ────────  ── dim 1 ...
```

If a cluster has 1764 vectors (avg in K=1700 IVF), that's 220 full panels
plus a partial. The partial panel pads with sentinel values (max int16
absolute, e.g. `INT16_MAX`) so the unused lanes' computed dist² is huge
and never wins the top-K.

## How dist_sq changes

The new inner loop processes 8 vectors per panel, in parallel inside
the SIMD register:

```cpp
// query is already a small int16[14] in a local var
// We compute 8 dist² values into one __m256i accumulator (8 int32 lanes).

__m256i acc = _mm256_setzero_si256();  // 8 int32 lanes, one per vector

for (int d = 0; d < 14; ++d) {
  // broadcast query[d] into all 8 lanes
  __m256i q = _mm256_set1_epi32(int32_t(query[d]));

  // load 8 int16 of dim d for v0..v7 (16 bytes, half a YMM)
  __m128i v_lo16 = _mm_loadu_si128((__m128i*)(panel + d * 16));

  // sign-extend each int16 → int32 (so subtract doesn't wrap on the wide diff)
  __m256i v = _mm256_cvtepi16_epi32(v_lo16);

  __m256i diff = _mm256_sub_epi32(v, q);
  acc = _mm256_add_epi32(acc, _mm256_mullo_epi32(diff, diff));
}

// At this point, acc has 8 int32 dist² values — one per vector in the panel.
// No horizontal reduce needed.

// Compare all 8 against current top_d[K-1] (broadcasted) in one shot:
__m256i thresh = _mm256_set1_epi32(int32_t(top_d_k_minus_1));
__m256i mask   = _mm256_cmpgt_epi32(acc, thresh);  // -1 in lanes "too big"
int     bits   = _mm256_movemask_epi8(mask);

// bits has 32 bits (4 per int32 lane). For every lane with all bits clear,
// the corresponding vector's dist² is BELOW threshold → candidate for top-K.
// For lanes with all bits set, the vector is over → skip.
```

Two things to notice:

1. **No horizontal reduce.** The 8 lanes ARE the 8 dist² values. They
   never need to be folded.

2. **One compare-vs-threshold replaces 8.** A single `vpcmpgtd` followed
   by `vpmovmskb` extracts which of the 8 candidates are below the cap.
   Today, AoS does this scalar compare 8 separate times — 8 branches
   that the predictor may or may not get right.

The math throughput is similar (we're still doing 8 × 14 = 112 SIMD ops
per panel either way), but the *overhead* shrinks substantially.

## Where the wins come from, in numbers

Estimated instruction counts per 8 vectors processed:

| step                            | AoS (today) | SoA-block |
|---------------------------------|-------------|-----------|
| Load vector data                | 8 × `vmovdqu` | 14 × `vmovdqu` (half-width) |
| Subtract + multiply-add         | 8 × (`vpsubw`+`vpmaddwd`) = 16 | 14 × (`vpsubd`+`vpmullo`+`vpaddd`) = 42 |
| Horizontal reduce               | 8 × ~5 instructions = 40 | 0 |
| Compare vs top_d                | 8 × 1 scalar `cmp` | 1 × `vpcmpgtd` + 1 × `vpmovmskb` |
| Branch on result                | 8 (each insert decision) | 1 broad branch (mask test) |
| **total ops, approx**           | **~72 ops** | **~58 ops** |
| **wasted lanes per panel**      | 16 (1 per vector × 8) | 0 |

The instruction count is similar, but the AoS version has 40 instructions
of *useless work* (horizontal reduce) every iteration. SoA replaces them
with productive work (more dim accumulations).

The bigger win is **branch behavior**: AoS has one unpredictable branch
per vector. SoA has one branch per 8 vectors. Branch mispredict cost on
Haswell is ~15-20 cycles; cutting 7 of 8 of them is large.

## The killer: early-exit at 8 of 14 dimensions

This is the trick that makes the SoA approach truly shine, and it doesn't
work cleanly with AoS.

Observation: in the top-K scan, once `top_d[K-1]` has been seated from
a few real candidates, the **threshold is small**. Most subsequent
vectors will be way above it. We don't need their exact dist² — we just
need to know they're above the threshold.

With SoA-block, you can run the dim-loop **partially**, check the
threshold, and skip the rest if no candidate is plausible:

```cpp
__m256i acc = _mm256_setzero_si256();
for (int d = 0; d < 8; ++d) {              // first 8 dims only
  // ... accumulate partial dist² ...
}

__m256i thresh = _mm256_set1_epi32(int32_t(top_d_k_minus_1));
__m256i over   = _mm256_cmpgt_epi32(acc, thresh);

if (_mm256_testc_si256(over, _mm256_set1_epi32(-1))) {
  // ALL 8 vectors already exceed threshold using just 8 dims —
  // their final dist² (after 14 dims) will be >= partial, so still over.
  // Skip the remaining 6 dim loads + work.
  continue;
}

// At least one candidate is plausible — finish the remaining 6 dims:
for (int d = 8; d < 14; ++d) {
  // ... accumulate ...
}

// final compare, insert into top-K
```

**Why this is valid**: dist² is a sum of *non-negative* terms (each is
`(query[d] - v[d])²`). The partial sum after 8 dims is a **lower bound**
on the final dist². If the lower bound already exceeds the threshold,
the full sum will too. Mathematically airtight.

**Why this saves a lot**: in a sorted cluster scan after warmup, ~80% of
panels (i.e. ~80% of the 8-vector groups) get fully rejected at this
check. Those panels skip:

- 6 dim loads (12 cache lines untouched)
- 18 SIMD ops (the dim-8..13 sub/mul/add chain)

Effectively cuts compute by ~40% on the cluster scan path, which is the
dominant cost. **This is the engine of the 50× p99 advantage in the
reference Rust submission.**

**Why this doesn't work cleanly in AoS**: with AoS, you've already loaded
the whole 16-int16 vector into a register. You can compute partial dist²
by masking, but you can't "save" anything — the load already happened,
the data is already in the L1 cache. The horizontal reduce is also still
needed to compare. The gain in AoS is much smaller (maybe 10%) and the
branch is per-vector (unpredictable).

In SoA-block, the early-exit decision is **before** loading the dim-8..13
slices of the panel. You save real cache traffic.

## Visualizing cache behavior

For a cluster scan of 1764 vectors:

**AoS today:**
```
56 KB total
(14 dims × 2 bytes + 4 bytes pad) × 1764 = 56,448 bytes
Read sequentially, 32 bytes per vector.
```

**SoA-block:**
```
49 KB total
14 × 2 bytes × 1764 = 49,392 bytes (no padding)

Read pattern with early-exit:
  panel 0:  read 224 bytes (full)
  panel 1:  read 128 bytes (8/14 dims, then skip)  ← if early-exit triggers
  panel 2:  read 224 bytes (full)
  ...
```

If 80% of panels early-exit after 8 dims, effective bytes read:
```
0.2 × 224 + 0.8 × 128 = 147 bytes per panel
220 panels × 147 = 32 KB read

Reduction: 56 KB → 32 KB = ~43% less cache traffic.
```

On an Haswell with 256 KB L2 and a tight CFS quota, halving the bytes
moved through L2 is a substantial fraction of the request time.

## What changes in the code

### `native/k_means.cpp` — writer

After clustering and assigning vectors, sort vectors by cluster, then
for each cluster split into panels of 8 and write dim-major:

```cpp
for (uint32_t c = 0; c < K; ++c) {
  // gather vectors of cluster c
  vector<int16_t*> cluster_vecs = ...;

  // pad to next multiple of 8 with sentinels
  size_t panel_count = (cluster_vecs.size() + 7) / 8;
  // ... pad with INT16_MAX values for unused lanes ...

  for (uint32_t p = 0; p < panel_count; ++p) {
    // write 14 dims × 8 int16 each
    for (uint32_t d = 0; d < DIM; ++d) {
      for (uint32_t lane = 0; lane < 8; ++lane) {
        size_t vidx = p * 8 + lane;
        int16_t v = (vidx < cluster_vecs.size())
                      ? cluster_vecs[vidx][d]
                      : INT16_MAX;     // sentinel for padding lanes
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
      }
    }
  }
}
```

The header gains a `panel_count` per cluster (the existing offsets array
can be redefined to index panel boundaries instead of vector boundaries).

Labels stay in the original per-vector array — they're 1 byte each and
not in the hot path.

### `native/ivf_index.hpp` — scan loop

`merge_top5_from_cluster` becomes panel-based:

```cpp
for (uint32_t p = 0; p < panel_count[c]; ++p) {
  const int16_t* panel = vectors + (panel_base[c] + p) * 224 / sizeof(int16_t);

  __m256i acc = _mm256_setzero_si256();
  for (int d = 0; d < 8; ++d) {
    __m128i v_lo = _mm_loadu_si128((__m128i*)(panel + d * 8));
    __m256i v    = _mm256_cvtepi16_epi32(v_lo);
    __m256i diff = _mm256_sub_epi32(v, _mm256_set1_epi32(query[d]));
    acc = _mm256_add_epi32(acc, _mm256_mullo_epi32(diff, diff));
  }

  // early-exit gate
  __m256i thresh = _mm256_set1_epi32(int32_t(top_d[TOP_K - 1]));
  __m256i over   = _mm256_cmpgt_epi32(acc, thresh);
  if (_mm256_testc_si256(over, _mm256_set1_epi32(-1))) continue;

  // finish remaining dims
  for (int d = 8; d < 14; ++d) { /* same as above */ }

  // 8 int32 dist² in acc — extract and insert candidates below threshold
  alignas(32) int32_t dists[8];
  _mm256_store_si256((__m256i*)dists, acc);

  for (int lane = 0; lane < 8; ++lane) {
    if (dists[lane] >= top_d[TOP_K - 1]) continue;
    uint32_t vec_idx = panel_base[c] + p * 8 + lane;
    if (vec_idx >= cluster_size[c]) continue;  // sentinel pad lane
    // insertion sort into top-K — same as today
  }
}
```

### Binary format

Bump from current `IVF1` int16 format to something like `IVF2` with a
new layout. The loader detects the magic and dispatches.

To stay backward-compatible during transition, support both formats:
the existing `dist_sq` becomes the fallback, the new SoA path is on the
`IVF2` files only.

## Implementation effort estimate

| piece                                            | effort  |
|--------------------------------------------------|---------|
| Format magic + header parsing                    | 30 min  |
| `k_means.cpp` writer (panel layout + sentinels)  | 1-2 hrs |
| `ivf_index.hpp` SoA `dist_sq` + scan loop        | 2-3 hrs |
| Fallback path / version dispatch                 | 30 min  |
| Retrain + verify (smoke + sweep)                 | 1 hr    |
| Submit + iterate                                 | -       |
| **total**                                        | **5-7 hrs** |

## Expected gain

Lower bound: ~30% reduction in cluster-scan CPU time, mostly from
eliminating horizontal reduce overhead. Score impact: ~+300-500 pts on
the p99 side (each 10× p99 cut is +1000 pts; going from 2 ms to 1 ms
is +300 pts).

Upper bound: with early-exit working as advertised (~80% of panels
rejected at 8 dims), the cluster-scan is ~2-3× faster. p99 could drop
toward sub-1ms, capping p99_score at 3000. That would bring final
score from current 5485 (VM) toward the 5800+ region — within striking
distance of the 6000 ceiling.

## What this does NOT change

- The IVF structure itself (clusters, centroids, top_n_centroids)
- The adaptive nprobe logic (FAST_NPROBE=5 with re-run on borderline)
- The HTTP layer (Ractor pool stays the same)
- The detection accuracy (top-5 ordering is preserved exactly — we're
  just computing the same dist² values in a different SIMD pattern)

This is purely an inner-loop optimization on the cluster scan, plus a
mathematically-safe early-exit. No algorithmic change to the detection.
