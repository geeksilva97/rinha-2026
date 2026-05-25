# Profiling findings — where the time goes

Real numbers collected on a Mac-mini-2014-equivalent GCP VM under the
official rinha k6 load (120s ramp to 900 RPS, 250 max VUs, 2001ms
per-request timeout). See [profiling-methodology.md](./profiling-methodology.md)
for setup details.

## Top-line numbers (per-stack, k6 results)

Three Puma runs, three Ractor runs, same VM, back to back:

| stack  | final_score | p99      | http_errors |
|--------|------------:|---------:|------------:|
| Puma single-mode workers=0 threads=4 | 3104, 3132, 3094 | 786 / 738 / 806 ms | 0 / 0 / 0 |
| Ractor-per-request                   | 2964, 2860       | 1085 / 1381 ms    | 0 / 0     |

For comparison, the real Mac mini results from the rinha runner:

| stack         | final_score | p99      |
|---------------|------------:|---------:|
| Puma (#6400)  | 2264        | 1812 ms  |
| Ractor (#6443)| 2790        | 1620 ms  |

VM is friendlier than the real Mac mini (faster Xeon, kernel page cache
that fits the IVF index easily) but the *shape* of the bottleneck
reproduces: "Insufficient VUs, reached 250 active VUs and cannot
initialize more" — same warning the real Mac mini test hits.

## Layer 1 — cgroup throttling

Aggregated from `cpu.stat` deltas across the full 120s test:

| run        | cpu_used (of 0.4 CPU limit) | wall throttled  | periods @ ceiling |
|------------|-----------------------------|-----------------|-------------------|
| puma-r1    | 30.8% of 1 core             | 42.2% wall      | 725 / 1214        |
| puma-r2    | 30.9%                       | 43.4%           | 727 / 1210        |
| puma-r3    | 31.0%                       | 43.5%           | 747 / 1212        |
| ractor-r2  | 30.1%                       | 113.1% (thread) | 728 / 1217        |
| ractor-r3  | 30.1%                       | 103.7% (thread) | 707 / 1211        |

### The "I don't see CPU at the limit" paradox

`docker stats` shows ~20% CPU during the test — well under the 0.4 CPU
limit. **The container isn't sustainedly saturated; it's pulsed.**

`cpu.stat` reveals the real picture: the test ran through ~1210 CFS
periods of 100ms each. Of those, **~60% hit the ceiling and got
throttled mid-period**. The other ~40% finished their work with quota
to spare. Averaged over time, utilization looks low. Within bursts,
it's pinned.

This matters because throttling kills tail latency: when a request
arrives mid-throttle, it waits for the next period to refill (up to
60ms of dead time), then potentially several more periods if the
queue keeps refilling.

### Throttling >100% on Ractor

Throttle wall time of >100% looks impossible until you remember
`throttled_usec` is summed across **all threads** in the cgroup. The
Ractor server spawns one OS thread per request — at 900 RPS the
container has many simultaneous threads, several of which can be
throttled at once. Each adds to the counter. Puma serial-mode keeps
one worker thread, so its throttle counter never exceeds wall time.

## Layer 2 — per-request distribution (nginx access log)

Sampled 40280 (Puma) and 35014 (Ractor) requests:

| percentile             | Puma       | Ractor       |
|------------------------|-----------:|-------------:|
| `request_time` mean    | 343 ms     | 435 ms       |
| `request_time` p50     | 484 ms     | **95 ms**    |
| `request_time` p95     | 660 ms     | 1395 ms      |
| `request_time` p99     | **688 ms** | 1574 ms      |
| `request_time` max     | 763 ms     | 2002 ms      |
| `queue_wait` (LB)      | < 1 ms     | < 1 ms       |

### Where the time is NOT

`request_time ≈ upstream_response_time` in both stacks → nginx itself
**adds no queue**. The 200ms+ delay is entirely on the backend.

### The bimodal distribution

Histograms of `request_time` (ms):

**Puma:**
```
   1-5    ms:  11471  (28.5%)
   5-10   ms:    366  ( 0.9%)
  10-50   ms:   1688  ( 4.2%)
  50-100  ms:    826  ( 2.1%)
 100-500  ms:   8094  (20.1%)
 500-1000 ms:  17835  (44.3%)  ← bulk
```

**Ractor:**
```
   1-5    ms:  14460  (41.3%)  ← median is fast
   5-10   ms:    740  ( 2.1%)
  10-50   ms:   1602  ( 4.6%)
  50-100  ms:    769  ( 2.2%)
 100-500  ms:   6117  (17.5%)
 500-1000 ms:   2724  ( 7.8%)
1000-2000 ms:   8600  (24.6%)  ← bad tail
```

**Reading the shapes:**

- Puma single-mode is serial. Every request pays roughly the same
  scheduling cost. Distribution is narrow but centered at 500ms.
  Predictable but mediocre.

- Ractor parallelizes. Most requests pass through fast (41% under 5ms!)
  but some get stuck in the throttle queue and pay 1-2s. **Better
  median, worse tail.**

The real Mac mini sees the same effect — Ractor's better median wins
on detection_score (got 3000 max — zero misses), but the long tail
keeps p99 close to the 2000ms timeout.

## Layer 3 — `perf` CPU profile (Ractor stack)

`perf record -F 99 --call-graph fp -G <cgroup>` during a 110s window
captured 2977 samples from api1 (matches the ~30% CPU utilization —
99Hz × 110s × 0.3 = ~3270 expected).

### Hot region: `ivf::ivf_score`

The function starts at offset 0x5500. Top samples cluster within
~0x150 bytes of that:

| offset | %     | what it's doing                              |
|--------|------:|----------------------------------------------|
| 0x55d9 |  7.8% | `vsubps` — SIMD load + subtract (dist_sq)    |
| 0x55e0 |  6.0% | continued SIMD ops                           |
| 0x5603 |  2.3% | `cmp top_d[K-1]` (fast-reject)               |
| 0x5608 |  4.4% | insertion-sort shuffle into top-K            |
| 0x5612 |  2.8% | comparison                                   |
| 0x5622 | 10.2% | SIMD reduction (`vextractf128 + vaddps`)     |
| 0x562a | 17.7% | final reduce + comparison                    |
| 0x5635 |  6.0% | top-K update                                 |
| **sum**| **~51%** | **of all CPU time in this function**     |

(Disassembly excerpt around the hot loop in
`ext/fraud_index/fraud_index.cpp` → calls `ivf::ivf_score` →
`merge_top5_from_cluster` → `ivf::dist_sq`.)

The remaining ~49% spreads across:
- ~10% in `top_n_centroids` (the K=1700 centroid scan to pick nprobe)
- libc memcpy/memset (Ruby/Ractor lifecycle)
- kernel CFS scheduler overhead during throttle wakeups

### Why ivf_score dominates

With nprobe=70, K=1700, mean cluster size = N/K = 3M/1700 ≈ 1764:

```
vectors visited per query  = 70 × 1764 = 123,529
bytes per vector           = 14 floats × 4 = 56 bytes
total memory per query     = 123,529 × 56 ≈ 6.9 MB
```

Each cluster is 1764 × 56 = **99 KB** → fits in L2 (256KB on Haswell)
but **not** in L1 (32KB). Every vector pulled from L2 → ~12-cycle
latency. AVX2 dist_sq is ~3-5 cycles of pure compute. **The inner
loop is memory-latency bound**, not compute bound.

## Conclusion: where to optimize

The CFS throttling is downstream of how much CPU each request
demands. We can't widen the quota. We can only make each request
cheaper.

| lever                                | expected impact                         | risk |
|--------------------------------------|------------------------------------------|------|
| K=4096 + nprobe ≈ 192 (same %varrida) | cluster shrinks to 41 KB → ~fits L1; ~3× faster dist_sq inner loop | low, just retrain |
| pre-compute ‖v‖² offline             | save the SIMD subtract+square; ~15-20% per dist_sq | medium, format change |
| Product Quantization (int8)          | 4× SIMD throughput (32 int8 per AVX2 reg) | high, precision loss |
| C++ HTTP server (cpp-server-experiment branch) | remove Ruby/Ractor layer | medium, already prototyped |

Starting with **K=4096 retrain** — lowest risk, biggest expected win
on this workload, and validates the cache-locality hypothesis
quantitatively.
