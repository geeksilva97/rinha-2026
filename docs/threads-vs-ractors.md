# Threads vs Ractors — why the prediction was wrong

We had a hypothesis that Ractors would beat Threads on this workload
because Ractors have per-VM GVLs and can run C-extension code in
parallel. The empirical numbers on the VM say otherwise — at the same
ACCEPTORS, a vanilla `Thread + Thread::Queue` pool beats the
`Ractor + Port` pool by ~900 score points and cuts p99 ~5×. This doc
records the experiment and unpacks why the model was off.

## The data

Same VM (n1-custom-4-8192, Haswell, 4 vCPU, 8 GB), same docker-compose
constraints (api1+api2: 0.4 CPU and 160 MB each, lb 0.2/30), same
adaptive nprobe (FAST=5, FULL=70), same int16 IVF index. The only
difference is the server module:

- `server_ractor.rb` — pool of `ACCEPTORS` worker Ractors, main thread
  accepts and `worker.send(fd)`.
- `server_threadpool.rb` — pool of `ACCEPTORS` worker Threads pulling
  from a shared `Thread::Queue`, main thread accepts and `queue.push(fd)`.

5-run averages from the VM:

| concurrency | ACCEPTORS | score | p99 |
|---|---|---|---|
| Ractor pool | 1 | 3052 | 585 ms |
| **Ractor pool** | **2** | **3113** | **514 ms** |
| Ractor pool | 4 | 2961 | 723 ms |
| Ractor pool | 8 | 2940 | 759 ms |
| (above ran without adaptive nprobe; adaptive added later) | | | |
| Ractor pool + adaptive | 2 | 4617 | 10.6 ms |
| **Thread pool + adaptive** | **2** | **5486** | **2.15 ms** |
| **Thread pool + adaptive** | **4** | **5529** | **1.96 ms** |
| Thread pool + adaptive | 8 | 5571 | 1.77 ms |

The same adaptive-nprobe shift that took Ractors from 3113 → 4617 took
Threads to 5500+. The headroom over Ractors is **~900 score / 5× p99**.

## What I expected and why

The prediction model said Threads would underperform under heavy C-extension
load because:

1. `FraudIndex.fraud_count_payload` is a C call that holds the GVL for
   ~500 µs (the SIMD inner loop). With many concurrent workers, only one
   can be inside that call at a time; the others wait in line for the GVL.
2. GC is stop-the-world across all threads.
3. Ractors have per-VM GVL → no GVL contention; per-Ractor heap → no
   GC stalls reach other Ractors.

That's all true *in isolation*. But it ignored what the actual environment
looks like.

## Why the model was wrong

### CFS quota already serializes the compute

The api container has `cpu.max = 40000 100000` — 40 ms of CPU per 100 ms
wall window. **You cannot run more than 0.4 CPUs worth of compute total,
regardless of how many threads/Ractors exist.** Even with 4 Ractors each
having their own GVL, all four share the cgroup quota; the kernel
scheduler time-slices them.

So under CFS-pressure:
- GVL serialization (threads) ≈ CFS serialization (Ractors). Same
  total compute throughput.
- The GVL "wasted holds" I feared don't matter when there's no idle
  CPU to waste them on.

### Ractor.send + Port.receive isn't free

Ruby's Ractor IPC, even between Ractors in the same process, has
non-zero overhead for each round trip:
- The argument is checked for shareability and possibly copied/moved.
- The target Ractor's port has its own lock/wakeup machinery.
- The scheduler crosses between Ractors, which is more expensive than
  a plain GVL handoff between threads of the same Ruby VM.

`Thread::Queue#push/pop` is a single in-process mutex with a condvar.
A few hundred ns at most per operation.

At 900 RPS, that's ~900 IPC round-trips per second per container.
Ractor overhead × 900 ≈ measurable. Queue mutex × 900 ≈ negligible.

### Per-Ractor heap multiplies memory pressure

Each Ractor in the pool has its own object heap. With 2-4 Ractors per
container in 160 MB cgroup, you have 2-4 small heaps churning instead
of one consolidated heap. Net effect: more frequent minor GCs across
the system. The "no stop-the-world across Ractors" win is theoretical
upside, but the "multiple small heaps each doing their own GC" is
practical downside.

A Thread pool shares one heap — fewer total collections, even though
each one stops all threads briefly. Brief stops on a process that
spends 95 % of its time inside C code (where GVL is held) are
basically invisible.

### The I/O hiding pipelining was just as good with Threads

The original hypothesis was: while one worker is in a syscall
(`accept`, `readpartial`, `write`), another can be doing compute.
Ractors and Threads both achieve this — both release the GVL during
blocking I/O. The difference between them only shows up when the
**non**-I/O work has true CPU parallelism, which CFS denies us anyway.

## What this tells us about the heuristics

**"Always reach for Ractor for performance" is wrong.** Ractors are
the right tool when you have:
- Genuine parallel CPU (no cgroup quota throttling everything)
- Long compute regions that hold the GVL (much longer than 500 µs)
- Need for isolation (memory safety between workers)

For this workload, none of those apply:
- CFS quota is the bottleneck, not the GVL
- The C call is short
- All workers share the same read-only index — isolation is unnecessary

A vanilla Thread pool with `Thread::Queue` is simpler, faster, and uses
less memory here. It also avoids the small but real ecosystem fragility
of Ractor (Ractor API is still marked experimental in Ruby 4.0, some
gems and Ruby builtins are not Ractor-safe).

## The corrected mental model

Concurrency primitive choice is best understood as:

```
cost-per-handoff × handoffs-per-second + benefits-from-parallelism
```

For us:
- handoffs/sec = ~900 (one per request)
- cost-per-handoff: Threads << Ractors
- parallelism gain: 0 (CFS-capped) for both

So Threads win by the handoff cost alone. The GVL would only matter if
we had idle CPU sitting unused — and we don't.

If we ever escape CFS throttling (e.g. competition allows 4 CPUs per
container), the math flips: parallelism gain becomes large, GVL becomes
the bottleneck for Threads, and Ractors pay off. We're not in that
world.

## See also

- `server_ractor.rb`, `server_threadpool.rb` — the two implementations.
- `docs/profiling-findings.md` — where we identified CFS throttling as
  the real bottleneck.
- `docs/next-optimizations.md` — what's left after this experiment.
