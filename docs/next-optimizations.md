# Remaining optimizations

Reference state after issue #6556 (official rinha test on Mac mini 2014):

| metric            | value     |
|-------------------|-----------|
| `final_score`     | **4362.78** |
| `p99`             | 18.88 ms |
| `p99_score`       | 1724.01  |
| `detection_score` | 2638.76 (`rate_component` 3000, `absolute_penalty` −361.24) |
| false positives   | 6        |
| false negatives   | 3        |
| http errors       | 0        |
| failure rate      | 0.02%    |

Score ceiling under the rinha formula is **6000** (3000 + 3000). The two
independent components and what bounds them today:

```
score_p99 = 1000 · log10(1000 / max(p99, 1))
            = 1000 · log10(1000 / 18.88)
            ≈ 1724.

To reach 3000 → p99 ≤ 1 ms.
Each 10× drop in p99 buys +1000 points.

score_det = 1000 · log10(1 / max(ε, 0.001))     − 300 · log10(1 + E)
            = 3000                              − 300 · log10(1 + 15)
            ≈ 2638.

E = 1·FP + 3·FN + 5·Err = 6 + 9 + 0 = 15.
ε is below ε_MIN (0.001), so the rate term is already saturated at 3000.
Only the absolute_penalty term (−300 · log10(1+E)) remains.

To reach 3000 → E = 0 (zero misclassifications).
```

The whole story now is **−361 points lost to 9 detection mistakes** and
**−1276 points left on the p99 table**. Total headroom: **~1637 points**
without changing the scoring engine.

---

## 1. Eliminate detection mistakes (~+361 points)

**The bug:** the adaptive nprobe in `ext/fraud_index/fraud_index.cpp::fraud_count`
re-runs only when the fast pass returns 2 or 3 (the values that could flip
the 0.6 approval threshold). But fast=5 occasionally returns a non-borderline
result (0, 1, 4, 5) that the full nprobe=70 would have flipped. Those
become FP/FN.

Numbers: at fast=5 we visit ~5 × 1764 = 8 800 vectors per query. That is
0.29 % of the 3M total. For most queries that's enough; for the 0.02 %
in the data that sit in a sparse fraction of the index, the top-5 we
recover is the wrong top-5.

**Three knobs (cheap → expensive):**

1. **Widen the escalation band.** Today: counts ∈ {2, 3}. Try {1, 2, 3, 4} —
   anything that isn't 0/5 (clear consensus) re-runs at full. Expected:
   eliminates most FP/FN. Cost: ~2 extra runs for ~30 % of queries
   instead of ~5 %. Probably pushes p99 up 5–10 ms.

2. **Bump fast_nprobe.** Try fast=10 or fast=15. More vectors visited
   first time → fewer wrong "clear" answers. Cost: linearly more compute
   per request, even on the non-borderline path.

3. **Stable-by-construction adaptive.** Run fast=5 and full=70 and compare
   the top-5 vector indices. If they overlap < some threshold, the fast
   answer is suspect even when the count agrees. Adds full cost for
   borderline-by-similarity queries, not just borderline-by-count.

Recommended start: (1). One-line change.

Files: `ext/fraud_index/fraud_index.cpp` — the `if (count == 2 || count == 3)`
guard around the re-run.

---

## 2. SoA layout in 8-vector blocks (~+300–500 p99 points)

Current layout (AoS): vectors stored contiguously dim-by-dim
`[v0.d0..v0.d13, v1.d0..v1.d13, ...]`. dist_sq processes one vector per
AVX2 register (16 int16 = one vector + 2 zero-padding lanes).

SoA-block layout: pack 8 vectors as 14 dims × 8 lanes per dim:
`[v0..v7].d0, [v0..v7].d1, ..., [v0..v7].d13`. Each block is 14 × 8 × 2 =
224 bytes (vs 8 × 32 = 256 bytes today, 12 % less memory).

Per-block scan: broadcast query.d[i] to a 16-int16 register (`_mm256_set1_epi16`),
load 8 stored values for that dim (128-bit `vmovdqu`), `vpsubw`, then
`vpmaddwd` paired with itself for sum-of-squares. After 14 dims, the
accumulator holds 8 int32 dist² values — one per vector in the block.

**Why it wins**
- One block load (224 B) processes 8 vectors. AoS today loads 32 B per
  vector → 256 B for 8.
- All 8 lanes are real data, no padding wasted (vs DIM=14 / STRIDE=16 today).
- The HW prefetcher sees fewer, larger sequential reads.
- The `vpmaddwd` output of 8 distinct dist² per block lets us reject
  whole blocks with a single `_mm256_cmpgt_epi32` against `top_d[K-1]`.

**Cost**
- Training (`native/k_means.cpp`) rewrites the vectors section in block-SoA
  order at the end. Index file gets a new section but stays the same size
  ± padding.
- `ivf_index.hpp::merge_top5_from_cluster` is the most affected — it now
  iterates over blocks of 8 vectors, not individual vectors. Insertion-sort
  into top-K stays the same, but you batch-test 8 candidates each iteration.
- Cluster boundaries may not be multiples of 8 — pad last block with
  sentinel values (max int16 squared, sums to INT64_MAX) so they never
  enter the top-K.

**Expected impact** on the dist_sq inner loop: ~1.5–2× throughput.
Combined with the early-exit hack below, more like 3–4×.

---

## 3. Early-exit SIMD mask after 8 dims (~+200–400 p99 points)

In the SoA block, compute partial dist² across the first 8 dims only.
Compare each of the 8 lanes against the current `top_d[K-1]` threshold
(after the early queries warm it up):

```cpp
__m256i partial = /* sum of 8-dim squared diffs across 8 vectors */;
__m256i thresh  = _mm256_set1_epi32((int32_t) min(top_d[K-1], INT32_MAX));
__m256i over    = _mm256_cmpgt_epi32(partial, thresh);
if (_mm256_testc_si256(over, _mm256_set1_epi32(-1))) continue; // all 8 over → skip
```

When the threshold is tight (after the first ~hundred queries inside the
cluster scan), most candidates exceed it on the first 8 dims and never
need the final 6. The reference rust submission claims this is the
biggest constant-factor win after SoA.

**Implementation note.** Only kicks in after a few entries of `top_d`
are real (not INT64_MAX). Either prime `top_d` from a coarse pass over
the cluster center, or just let the first few candidates per cluster
go full-distance.

**Stacks with #2.** Without #2 the comparison only saves you 6 dims out
of 14 per vector, and the branch is unpredictable. With block-SoA, you
test 8 vectors per `vpcmpgtd` and skip them as a group, which the
predictor handles well.

---

## 4. Drop the Ruby layer entirely (~+100–200 p99 points)

The `cpp-server-experiment` branch (`native/server.cpp`) already exists.
It's a picohttpparser + fork() workers server that bypasses Ractor.new,
GVL, `IO.for_fd`, the Ruby HTTP parsing all together. Local lab tests
showed ~5450 score / p99 3.5 ms on the M4, with the same fraud_index
calls. On the Mac mini with throttled CPU it would save ~200–300 µs per
request.

**Stacks with everything above.** This is the final layer to strip.

**Cost.** Reviving the branch, getting it past the rinha runner's
healthcheck (the `cpp-server` Dockerfile is separate; need to wire
nginx the same way), and absorbing the maintenance cost of a hand-rolled
HTTP path. Higher risk than #1–3.

---

## Priority order

If chasing the score ceiling:

1. **#1 — widen escalation band.** Cheapest, recovers ~+361 detection points,
   essentially free behavior change.
2. **#2 + #3 together.** SoA + early-exit. Biggest p99 lever, ~+500–800
   points combined. Significant code rewrite in `ivf_index.hpp` and
   `k_means.cpp` output stage. Index file format change → retrain required.
3. **#4 — cpp-server.** Final ~+100–200, mostly p99. Branch already
   sketched.

Realistic with #1+#2+#3 in this design: score ~5400, p99 ~3–5 ms.
Adding #4 should push toward the 5800–6000 ceiling.

Hard floor that we cannot escape on this design:
- 1 CPU shared across api1 + api2 + lb on a 2014 Haswell mobile chip.
  At 900 RPS, ~1.1 ms CPU per request available. Anything under that
  is impossible to sustain; anything close to it leaves headroom for
  the CFS bursts that show up as p99 tail.

---

## Things explicitly NOT worth doing (already explored)

- **Larger K (e.g. K=4096).** Tested in issue #6496 era. Cluster size
  shrinks but centroid scan grows linearly. Net wash on this hardware.
- **Prefetch in the cluster scan.** Tested briefly, caused L1 cache
  pollution and regressed score. The HW prefetcher already handles the
  sequential cluster reads.
- **`Connection: keep-alive` between LB and backend.** Already configured
  in `nginx.conf` (`upstream api { keepalive 64 }`). Per-request
  reconnection is not on the hot path.
- **`-O3 -funroll-loops`.** Already on (`ext/fraud_index/extconf.rb`).
- **More Puma workers / threads.** Confirmed irrelevant — GVL serializes
  C-extension calls. Ractor pool is the chosen concurrency primitive.
