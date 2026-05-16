# Rinha Fraud Detection

Rinha de Backend-style challenge. A fraud-scoring HTTP endpoint runs behind a load balancer with **two app instances**.

## Resource budget (HARD limits)

- **Total memory: 350MB** across the whole stack (LB + both app instances)
- **Total CPU: 1.0** divided between LB and the two app instances (they sum to 1)

Every design decision must respect this. Per-instance budget is roughly ~120–150MB RAM and ~0.45 CPU.

## Host environment

- **OS:** Ubuntu 24.04 (x86_64 unless noted)
- **Host hardware:** 2.6 GHz CPU, 8 GB RAM, 1 TB storage
- **Docker:** runs natively on Linux (no Docker Desktop VM layer) — container CPU/RAM limits are enforced directly by the kernel via cgroups.
- **Effective per-instance CPU:** ~`2.6 GHz × 0.45 ≈ 1.17 GHz` of single-thread time. No turbo headroom assumed. Sloppy Ruby allocations in the request path will be visible in p99.
- **Offline ingest & ANN training:** run on the host (8 GB RAM is plenty for `Oj.load` of the 285 MB JSON ~2 GB peak, plus the training step) or in a one-off unconstrained container. Never inside the limited app container.

## Stack

- Ruby 4.0.2, Rack 3.2, Puma 8.0, Oj 3.17
- Endpoint: `POST /fraud-score` — accepts a transaction payload, returns a fraud score
- `lib/to_vec.rb` maps the payload to a feature vector
- Scoring: kNN over a labeled dataset using SQLite's `vec1` extension

## Architecture plan

### Deployment model

The container ships with a **fully prebuilt, pre-trained `fraud.db`**. Nothing about the dataset, the index, or training happens at container build time or runtime — only at app dev time, on the host.

```
host (8 GB RAM, unconstrained):
  vectors.json (285 MB)
     │
     ▼
  bin/ingest.rb   → INSERTs all vectors into fraud.db
     │
     ▼
  bin/train.rb    → vec1_train(...) → INSERT ('rebuild', :model)
     │
     ▼
  fraud.db (final, indexed, quantized — committed/artifacted)

docker build:
  COPY fraud.db /app/
  COPY vec1.so  /app/      (or build it in a builder stage — see below)

container runtime (350 MB / 1 CPU shared):
  open fraud.db read-only, query. Never trains, never ingests.
```

The split exists because both ingest and training transiently allocate hundreds of MB to GBs — impossible inside the 350MB total runtime budget. Doing them on the unconstrained host once and shipping the artifact sidesteps the whole problem.

### Dockerfile shape

Multi-stage build:

1. **`builder` stage**: install `build-essential` + sqlite headers, compile `vec1.c` against the target arch (x86_64 Linux), produce `vec1.so`.
2. **`runtime` stage**: minimal base (`ruby:4.0-slim` or similar), install `libjemalloc2` + `sqlite3`, `COPY` the app, `COPY` `fraud.db` and `vec1.so` from the builder stage. Set `LD_PRELOAD` for jemalloc.

This keeps the final image to: Ruby + libjemalloc + libsqlite3 + the app + the prebuilt DB + the compiled extension. No compilers, no dataset JSON, no training tooling in production.

### Data layer — SQLite + `vec1` extension

- Labeled dataset (~285MB JSON, vectors + `"legit"`/`"fraud"` label) is preprocessed **once, offline** into `fraud.db`.
- `fraud.db` schema: `CREATE VIRTUAL TABLE vectors USING vec1(vector, is_fraud);` — `vector` is a raw f32 blob (`array.pack("e*")`), `is_fraud` is `0`/`1`.
- Both Puma instances open `fraud.db` read-only. OS page cache is shared across processes — the dataset is paid for in RAM **once**, not per instance.
- **Use ANN with trained IVFADC + product quantization.** This is a *compression* decision, not just a speed one: raw f32 vectors at 14 dims cost 56 bytes/row in the page cache; OPQ at `codesize:8` brings each row down to 8 bytes (~7× smaller). On a ~1–2M row dataset that's the difference between ~100MB and ~15MB of resident memory — only the quantized version fits under the 125MB-per-instance budget.
- Training is done **once, offline** as part of the ingest pipeline, then `INSERT ... ('rebuild', :model)` installs the trained model into `fraud.db`. The server never trains; it only opens the prebuilt DB and queries.
- Query: `SELECT is_fraud FROM vectors(?, '{k:25, nprobe:8}')` with the packed query blob. Tune `k` and `nprobe` against the test set — `nprobe` is the speed/recall knob.
- Accept that results are approximate. The label vote is robust to a few missed neighbors, and the RAM win is non-negotiable.

**Never** parse the JSON dataset at boot or in the request path. JSON in, SQLite out, done once.

### App layer — Puma

Per-instance config (`puma.rb`):

```ruby
workers 0          # single process, no forks — duplicated heap blows the RAM budget
threads 1, 4       # small pool; sqlite3 releases the GVL during queries, so 2–4 threads overlap work
bind "tcp://0.0.0.0:9999"
```

- **No cluster mode.** Two workers per instance = two Ruby heaps = OOM under 125MB.
- **Small thread pool.** The `sqlite3` gem releases the GVL inside SQLite calls, so threads buy real overlap. More than ~4 threads just adds GVL contention and CFS throttling.
- **No `preload_app!`** — only matters in cluster mode.
- **No YJIT by default** — trades RAM for CPU; on a 125MB budget that trade is suspect. Benchmark before enabling.

### Memory allocator — jemalloc

Linux glibc malloc fragments badly under threads and refuses to release memory back to the OS — RSS climbs and stays. jemalloc cuts steady-state RSS by 20–40% with zero code changes. Mandatory under this budget.

In the Dockerfile:

```dockerfile
RUN apt-get update && apt-get install -y libjemalloc2 && rm -rf /var/lib/apt/lists/*

# Match the path to the image arch:
#   x86_64:  /usr/lib/x86_64-linux-gnu/libjemalloc.so.2
#   aarch64: /usr/lib/aarch64-linux-gnu/libjemalloc.so.2
ENV LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2
ENV MALLOC_CONF="narenas:2,background_thread:true,dirty_decay_ms:1000,muzzy_decay_ms:0"
```

`MALLOC_CONF` knobs explained:
- `narenas:2` — keep arena count low (low-thread workload, fewer arenas = less fragmentation)
- `background_thread:true` — reclaim memory async instead of on each alloc
- `dirty_decay_ms:1000`, `muzzy_decay_ms:0` — return freed pages to the OS quickly (default 10s/10s holds RSS high for no reason)

Verify it actually loaded: `cat /proc/$$/maps | grep jemalloc` inside the container. If empty, the path is wrong for the arch.

Do **not** also set `MALLOC_ARENA_MAX` — that's the glibc knob and has no effect with jemalloc.

## Design rules (decision filters)

- **Heavy work happens offline.** Anything that costs real RAM or CPU must be precomputed into a file, not done at boot or per request.
- **Files over in-memory caches.** Shared read-only files are paid for once via the OS page cache; per-process caches are paid for twice.
- **Measure RSS before adding any gem or in-process cache.** The budget is small enough that one careless `require` can tip it.
- **Keep the request path single-threaded-friendly.** With ~0.45 CPU per instance, parallelism inside a request is a loss; sequential, allocation-cheap code wins.

## Stages of the work

**Offline (host, one-time, produces `fraud.db` artifact):**

1. **Decide the feature vector dimension and lock it.** `to_vec` currently returns 7; sample data hints at 14. The vec1 table's dimension is frozen by the first INSERT — re-ingest if it changes.
2. **Write `bin/ingest.rb`.** `Oj.load` the 285 MB JSON (8 GB host RAM handles the ~2 GB peak fine), pack each vector with `pack("e*")`, batch-insert into `fraud.db` inside transactions of ~50k rows. Set `PRAGMA journal_mode=OFF; synchronous=OFF` for the load only.
3. **Write `bin/train.rb`.** Run `vec1_train` over a sample (e.g. `LIMIT 100_000`) with `{distance:"l2", nbucket:512–1024, codesize:8, opq:true}`, then `INSERT ... ('rebuild', :model)` to install it. Verify the on-disk DB shrank and queries return sensible neighbors. The output `fraud.db` is the deployment artifact.

**App (container, runs against the prebuilt DB):**

4. **Wire up the query path.** Open `fraud.db` read-only at boot, prepared statement for the kNN query, pack the query vector per request, vote on the top-k labels.
5. **Multi-stage Dockerfile.** Builder stage compiles `vec1.so`; runtime stage `COPY`s `fraud.db` + `vec1.so` + the app, installs `libjemalloc2`, sets `LD_PRELOAD` and `MALLOC_CONF`, and runs `puma -C puma.rb`.
6. **Measure.** RSS per instance, p50/p99 latency, throughput under the CPU quota. Tune `k`, `nprobe`, thread count, and consider YJIT only if there's headroom.
