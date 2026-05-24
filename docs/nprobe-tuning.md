# nprobe tuning — IVF multi-probe sweep

## What nprobe does

In our IVF index (1700 centroids over 3M reference vectors), a query:

1. Computes distance to each of the 1700 centroids.
2. Picks the `nprobe` nearest centroids.
3. Brute-forces the top-5 nearest vectors **across all `nprobe` cluster's
   vectors** combined.
4. Returns `fraud_count / 5` as the score.

Lower `nprobe` = less work per query (less CPU, lower latency) but more
risk of missing the true top-5 because they straddle a cluster boundary.
Higher `nprobe` = more work, better recall.

## Methodology

GitHub Actions `ubuntu-latest` runner with the published GHCR image:
- 2× api containers, nginx LB, official rinha test (120s ramp to 900 RPS,
  54k requests against the official `test-data.json`).
- Multiple runs per `nprobe` to average out CI noise (variance ~±40ms p99).
- All runs validated zero HTTP errors.

## Sweep data

| nprobe | runs | errors | p99 range | p99 median | score range |
|---:|---:|---:|---:|---:|---:|
|  60 | 4 | always 1 FN | 1.6 – 372 ms (bimodal) | ~190 ms | 3249 – 5614 |
|  65 | 2 | always 1 FN | 1.6 – 39 ms | ~20 ms | 4226 – 5606 |
|  70 | 3 | 0 | 57 – 300 ms | ~81 ms | 4091 – 4242 |
|  80 | 1 | 0 | 352 ms | — | 3453 |
| 100 | 4 | 0 | 465 – 545 ms | ~480 ms | ~3322 |

The CI runner is shared hardware, so absolute p99 fluctuates ±40 ms. The
**relative** ordering between `nprobe` values is stable.

## Key findings

### 1. There is a single edge-case fraud at the cluster boundary

`nprobe ∈ {60, 65}` consistently miss **the same fraud** (1 FN, never more,
never different). The true top-5 for that query spans clusters 65–69. At
`nprobe=70` and above, those clusters are visited and the fraud is caught.

### 2. The latency cliff between 65 and 70

`nprobe=65` runs are dominated by ~1-40 ms p99. `nprobe=70` jumps to
60-300 ms. Hypothesis: at `nprobe ≥ 70` the working set exceeds the page
cache's effective size under the cgroup memory limit (160 MB per
container), so queries start page-faulting on cluster pages that get
evicted between requests. The 5 extra clusters at 70 cost ~5 × 95 KB =
~475 KB additional working set per query, which compounds across
concurrent queries.

### 3. Score trade-off

The rinha scoring weights errors heavily (`E = fp*1 + fn*3 + err*5`),
but caps the rate component at `epsilon_min = 0.001`. For a single FN
out of 54,059 requests:
- `epsilon = 3 / 54059 ≈ 5.5e-5` → capped to `0.001` for scoring.
- `rate_component = 1000 × log10(1/0.001) = 3000` (max).
- `absolute_penalty = -300 × log10(1+3) ≈ -180.6`.
- `detection_score ≈ 2819` (versus 3000 perfect).

The detection score loss from 1 FN is ~180 points. The latency score
gain from going `nprobe=65` (p99 ~20 ms → ~2740 points) vs `nprobe=70`
(p99 ~81 ms → ~2090 points) is ~+650 points.

Net: **`nprobe=65` wins by ~470 points** despite the single FN.

## Decision

`nprobe=65` selected as default. Trade 1 FN (always the same edge case)
for ~5× lower p99 and ~+470 score points.

If the official rinha eval (on dedicated Mac mini 2014 hardware) shows
different characteristics — e.g., no memory pressure, so the latency
penalty of `nprobe=70` disappears — revisit and switch to `nprobe=70`
for the perfect-detection bonus.

## How to change

Set `NPROBE` env var on the api containers. In `docker-compose.yml`:

```yaml
environment:
  - NPROBE=${NPROBE:-65}
```

Override at runtime:

```bash
NPROBE=70 docker compose up -d
```

The stress workflow accepts `nprobe` as a `workflow_dispatch` input:

```bash
gh workflow run stress.yml --ref main -f nprobe=70
```
