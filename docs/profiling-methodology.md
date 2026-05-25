# Profiling methodology

How we set up a Mac mini 2014 lookalike on GCP and profiled the stack
end-to-end to locate the bottleneck for the rinha test.

## Why a separate VM

Local tests on the M4 dev box gave score=5500/p99=3ms — nothing close
to the real Mac mini test environment (score=2264/p99=1812ms on
submission 6400). M4 has too much CPU headroom and an NVMe SSD that
absorbs page faults invisibly. The bottleneck only shows up under
the same constraints the rinha runs under, so we replicated those
constraints on a controlled VM.

## VM spec

```
Machine: n1-custom-4-8192
  - 4 vCPU = 2 physical Haswell cores with HT (matches i5-4278U)
  - 8 GB RAM (matches Mac mini)
  - min-cpu-platform = Intel Haswell (forces AVX2 baseline, no AVX-512)
Disk:   pd-standard, 30 GB
  (HDD-backed, ~150 IOPS — close to a Mac mini 2014 SATA SSD)
OS:     Ubuntu 24.04 LTS
Region: us-central1-a
Cost:   ~$0.12/hour
```

CPU info on the VM: `Intel(R) Xeon(R) CPU @ 2.30GHz`, AVX2/BMI2/FMA all
present. Mac mini 2014 i5-4278U boost is 2.6 GHz but sustained is closer
to 1.5-2.0 GHz under load — the VM is in the same ballpark.

## Stack constraints

The participant compose enforces:

| service | cpus  | memory |
|---------|-------|--------|
| api1    | 0.40  | 160 MB |
| api2    | 0.40  | 160 MB |
| lb      | 0.20  |  30 MB |
| total   | 1.00  | 350 MB |

Verified inside the VM that cgroup limits actually apply:
```
$ cat /sys/fs/cgroup/.../docker-<api1>.scope/cpu.max
40000 100000   ← 40ms CPU quota per 100ms period (= 0.4 CPU)
$ cat memory.max
167772160      ← 160 MB
```

k6 runs in its own container with `--cpus=2 --memory=8g --network=host`
— mirrors the rinha test harness exactly.

## Instrumentation layers

We instrumented at four levels:

### 1. k6 — top-level black-box

Same `test.js` the rinha uses. Output: `results.json` with
`final_score`, `p99`, `breakdown.http_errors`.

### 2. nginx access log — per-request timings

Override `docker-compose.profile.yml` swaps `nginx.conf` for
`nginx.profile.conf`, which adds a custom log format:

```
log_format prof '$msec $request_time $upstream_response_time $upstream_connect_time';
access_log /var/log/nginx/access.log prof buffer=64k flush=1s;
```

`upstream_response_time` is the full time the LB waited on a backend
(unix socket connect + send request + read response). `request_time`
is the same plus any time the LB spent on its own work. Difference =
queue inside nginx itself.

### 3. cgroup `cpu.stat` — CFS throttling

Sampled `/sys/fs/cgroup/system.slice/docker-<container>.scope/cpu.stat`
every 2s during the test. The key counters:

| field | meaning |
|-------|---------|
| `usage_usec`      | total CPU time consumed (µs) |
| `nr_periods`      | how many 100ms CFS periods elapsed |
| `nr_throttled`    | how many periods hit the quota and got throttled |
| `throttled_usec`  | total wall time the container spent throttled |

Diff between first and last sample gives the cumulative numbers
during the test. From those we derive:

- **CPU utilization** = `Δusage / (Δperiods × 100ms)` → 0.40 = saturated
- **Throttle ratio** = `Δthrottled_usec / (Δperiods × 100ms)` → fraction of wall time idle waiting for quota refill
- **Period saturation** = `Δnr_throttled / Δperiods` → fraction of periods that hit the ceiling

### 4. Linux `perf` — per-instruction CPU sampling

Sample-based CPU profile using kernel perf counters. The hard part on
docker containers is symbolization — perf records DSO offsets, so we
need debug symbols in the `.so`.

**Setup steps:**

1. Rebuild the extension with debug info — added `PROFILE=1` build
   arg in `Dockerfile`. When set, `extconf.rb` appends
   `-g -fno-omit-frame-pointer` to `$CXXFLAGS`. Production builds skip
   this; it's profiling-only.

2. Profile by **cgroup**, not by PID, so we capture Ractor spawns and
   all worker threads:
   ```
   perf record -a -F 99 -e cycles --call-graph fp,8192 \
       -G "system.slice/docker-<api1_cid>.scope" \
       -o api1.data -- sleep 110
   ```
   `-e cycles` must come before `-G` (a perf gotcha).

3. Copy the `.so` out of the running container into a `symfs/` tree
   that mirrors the in-container path:
   ```
   docker cp rinha-api1-1:/app/ext/fraud_index/fraud_index.so symbols/app/ext/fraud_index/fraud_index.so
   perf report --symfs symbols ...
   ```

If the symfs path doesn't match the container path the .so was at
(`/app/ext/fraud_index/fraud_index.so`), perf falls back to showing
hex offsets only. We hit this trap once.

## How to reproduce a run

```bash
# on the VM
~/perf_run2.sh ractor-perf3 docker-compose.ractor.yml
```

Inside, the script:
1. `git pull` to sync with main
2. `docker compose ... build --build-arg PROFILE=1`
3. `docker compose ... up -d --wait` (gated by healthcheck on the unix socket)
4. starts `perf record -a -G <cgroup>` for api1 + api2 in background
5. runs k6 in container against `localhost:9999`
6. on exit: copies `access.log`, `results.json`, `.so` for symbolization

Per run, output lives in `~/runs/<name>/` on the VM.
