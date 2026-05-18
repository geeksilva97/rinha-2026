# vec1 multi-threaded PQ encoding race during rebuild

Notes for filing — and maybe fixing — a real correctness bug in `sqlite.org/vec1`.

## Symptom

After training and rebuilding the index with `nthread > 1`, `PRAGMA integrity_check` deterministically fails on a single row:

```
vectors: %_idx PQ does not match calculated PQ for row 1971230
```

The specific row varies between runs (since training samples rows randomly and threads schedule non-deterministically), but exactly one row out of ~3M fails per run. With `nthread: 1`, integrity passes every time.

## Repro (this project)

```bash
# Clean ingest of the rinha-2026 references.json.gz dataset (3M rows × 14 f32)
rm -f fraud.db fraud.db-journal
bundle exec ruby bin/ingest.rb       # ~10s, produces fraud.db (~193 MB)

# Edit bin/train.rb: set nthread: 8 in CONFIG
bundle exec ruby bin/train.rb        # ~34s, integrity_check fails
```

Switch to `nthread: 1` and re-run train — passes (~89s).

A self-contained synthetic repro should also be possible: insert ≥ ~1M random 14-dim vectors into a `vec1(vector)` table, train+rebuild with `nbucket:1024, codesize:8, opq:true, nthread:8`, then `PRAGMA integrity_check`.

## What the integrity check is actually doing

`vec1.c:9806–9830` (`vec1IntegrityMethod`):

```c
u8 aPQ[VEC1_MAX_CODESIZE];
u8 aIdxPQ[VEC1_MAX_CODESIZE];

/* Recompute PQ for the row's vector */
vec1PqEncodeVector(pMod, aEnc, aPQ, 0);

/* Read the stored PQ from the index blob */
for(iCode=0; iCode<nCodebook; iCode++){
  aIdxPQ[iCode] = aBlob[iFrom + (iCode * VEC1_PQ_BLOCKSIZE)];
}

if( 0!=memcmp(aPQ, aIdxPQ, nCodebook) ){
  /* fail */
}
```

For PQ to be deterministic, recompute-from-source must always equal what was stored. The failure means **the stored code for that row was written incorrectly during the threaded rebuild**.

## Where to look

`vec1.c:8754` — `vec1QuantizeJob` (the worker function):

```c
static void vec1QuantizeJob(void *pCtx){
  Vec1QuantizeJob *p = (Vec1QuantizeJob*)pCtx;
  ...
  for(ii=0; ii<p->nVector; ii++){
    float *v = &p->aVec[nElem * ii];
    u8 *a = &p->aCode[nCodebook * ii];
    vec1QuantizeVector(p->pModel, p->aTmp, v, &p->aBucket[ii], a);
  }
}
```

Each job has its own `pQJ` buffers (`aVec`, `aCode`, `aBucket`, `aTmp` — see allocation at `vec1.c:8912–8934`), so per-job state is isolated. **But notice `p->aTmp` is a single `nElem`-float scratch buffer reused across all `VEC1_QUANTIZE_JOB_SZ = 1000` vectors in this job.**

That reuse is fine sequentially within one worker. The concern: is `vec1QuantizeVector`'s use of `aTmp` strictly write-then-read within a single iteration, with no leftover state from the previous iteration affecting the next?

Trace through `vec1QuantizeVector` (`vec1.c:8283`):

```c
aVec = vec1TransformInputVector(pMod, aTmp, aVector);
```

If there's no rotation configured, `vec1TransformInputVector` likely returns `aVector` unchanged and **does not initialize `aTmp`**. In that case `aTmp` carries stale data from the previous iteration. Later:

```c
if( pMod->hdr.nCodebook>0 && (pMod->hdr.flags & VEC1_MODEL_RESIDUAL) ){
  vec1Sub(aTmp, aVec, &pMod->aCentroid[iBucket*nElem], nElem);
  aVec = aTmp;
}
```

This **overwrites** `aTmp` with the residual. So even if `aTmp` had stale data, this would clobber it. Good.

If a rotation IS configured (`opq:true` in our config), `vec1TransformInputVector` writes the rotated vector into `aTmp`. Also fine.

Then:
```c
if( pMod->hdr.nCodebook>0 ){
  vec1EncodeVector(pMod, aVec, aCode);
}
```

Should write only `aCode`.

Per-job state looks clean. The race likely isn't in `vec1QuantizeJob` itself.

## More likely: the writer path or job-queue ordering

`vec1.c:8784` — `vec1QuantizeJobFinish` runs on the **main thread** (inside `vec1JobQueueFinishJobs` at `vec1.c:1461`), so writes to SQLite are serialized. That should be safe.

But the queue mechanism has main-thread-steals-work behavior (`vec1.c:1487` — if no finished job is ready, main thread grabs a waiting job and runs `xWork` inline). This means:
1. Worker A starts quantizing job 1
2. Main thread, waiting for finishes, grabs job 2 and runs `xWork` on it directly  
3. Main finishes job 2, then `xFinish`-es it → writes job 2's codes to idx
4. Worker A finishes job 1 → `xFinish` writes job 1's codes to idx

The writer is fed jobs out-of-order. Whether that's a problem depends on whether `vec1WriterQuantized` (`vec1.c:8806`) has order assumptions — e.g. assumes monotonic rowids, or shares cross-job buffer state.

That's the spot worth instrumenting first.

## Suggested investigation path

1. **Confirm determinism of the race.** Run the threaded rebuild 5–10 times in a row, log which rowid fails each time. If the failing rowid changes between runs, it's a true race. If it's always the same rowid, look at that specific row's vector data first.

2. **Bisect with thread count.** Does `nthread: 2` fail? `nthread: 4`? If failures scale with thread count, it's clearly contention. If `nthread: 2` is enough to trigger it, repro is much faster to iterate on.

3. **Instrument `vec1WriterQuantized`** to log every (rowid, bucket, code bytes) it receives. Run with `nthread: 8`, capture the log. Run with `nthread: 1` (or reference scalar encode), capture a log. Diff them. The first divergent row is the culprit; the difference (wrong bucket? wrong code? wrong rowid mapping?) tells you which buffer was raced.

4. **Inspect `Vec1Writer` for shared state.** The writer struct is shared across all jobs (`pQJ->pWriter = p` at line 8909, where `p` is allocated once before the loop). Any field that's written without synchronization is a candidate. Bucket builders (`p->aBld[iBld]`) look like they accumulate per-bucket state — if two jobs write to the same bucket from the writer's main-thread `Finish`, that's serialized. But if any pre-built per-job state (like `p->aPQ`, `p->aResidual`) is written during `xWork` running on a worker AND read/written during `Finish` for another job, you have a race.

5. **Compile with `-fsanitize=thread`** — ThreadSanitizer will pinpoint the racy access. The vec1 Makefile already has commented-out `-fsanitize=address,undefined`; adding `thread` (mutually exclusive with address) should reveal the bug if it's a true data race.

## Once a candidate fix is in hand

The vec1 test suite has good coverage:
- `vendor/vec1/test/vec1train.test` — training validation
- `vendor/vec1/test/vec1ac.test` — train + rebuild + query end-to-end
- `vendor/vec1/test/vec1pq.test` — PQ-specific

Run those with TCL after building, but **also add a deterministic threaded-rebuild stress test** that fails on current trunk. Use a fixed-seed RNG (vec1's tests use Tcl `expr srand(...)`) to generate ~1M vectors, train+rebuild with `nthread:8`, `PRAGMA integrity_check`, expect `ok`.

## Contributing back

vec1 lives on a Fossil repo at sqlite.org, not GitHub. Read `vendor/vec1/doc/` for any contributing notes. The SQLite team is responsive on the sqlite-users mailing list (`https://sqlite.org/forum/`). File the bug there with:

- The repro above (or a self-contained Tcl version)
- `vec1.c` checkin hash (from the tarball metadata)
- Host info (Apple M3, macOS 15.1, arm64) — but probably reproduces on any platform
- Observation that `nthread:1` works

If you submit a patch, expect feedback to focus on the test coverage as much as the fix itself. That's the SQLite team's style and it's a high bar.

## Why this is worth your time

- It's a real correctness bug in a serious project (sqlite.org).
- The fix is contained (the threading is well-bounded in `vec1RebuildIndex`).
- It's a great opportunity to learn a non-trivial multi-threaded C codebase with a clear failure case.
- A merged patch on a sqlite.org project is a respectable line on a portfolio.

The hardest part isn't the fix — it'll likely be one or two lines once located. The work is in narrowing down the racy access via TSan/instrumentation. Plan to spend a few hours bisecting before you see the cause.
