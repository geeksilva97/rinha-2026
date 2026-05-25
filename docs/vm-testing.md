# Testing on the GCP VM (Mac mini 2014 stand-in)

The rinha official tests run on a Mac Mini Late 2014 (2.6 GHz Haswell
i5, 8 GB RAM, Ubuntu 24.04). Local development on an Apple M4 doesn't
reproduce the bottleneck the test environment exposes (CPU saturation,
slow SATA storage), so we run all benchmarks on a GCP VM tuned to the
same profile.

This doc is operational — how to set the VM up, how to run a stress
test, how to read the metrics, and the gotchas you will hit. For the
profiling methodology and what each metric means, see
[profiling-methodology.md](./profiling-methodology.md).

## VM provisioning

GCP machine type matching the Mac mini's i5-4278U as closely as
possible: 4 vCPU = 2 physical Haswell cores with hyperthreading, 8 GB
RAM, pd-standard for storage (HDD-backed, ~150 IOPS — roughly the
slowest SATA SSD).

```bash
gcloud compute instances create rinha-mac-mini-2014 \
  --project=<your-project> \
  --zone=us-central1-a \
  --machine-type=n1-custom-4-8192 \
  --min-cpu-platform="Intel Haswell" \
  --image-family=ubuntu-2404-lts-amd64 \
  --image-project=ubuntu-os-cloud \
  --boot-disk-size=30GB \
  --boot-disk-type=pd-standard \
  --metadata-from-file=startup-script=/tmp/rinha-vm-startup.sh \
  --tags=rinha
```

Approximate cost: $0.12/hour (~$3/day). Don't forget to stop or delete
the instance when not in use.

### Startup script

`/tmp/rinha-vm-startup.sh` should install Docker, k6, gh CLI, and the
build essentials. Sketch:

```bash
#!/bin/bash
set -eux
exec > >(tee /var/log/startup.log) 2>&1

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
  ca-certificates curl gnupg lsb-release git make build-essential \
  python3 python3-pip dirmngr sysstat linux-tools-generic

# docker
install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
  -o /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo $VERSION_CODENAME) stable" \
  > /etc/apt/sources.list.d/docker.list
apt-get update
apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin

# k6 (direct binary — apt keyring step sometimes fails on first boot)
curl -sSL https://github.com/grafana/k6/releases/download/v1.0.0/k6-v1.0.0-linux-amd64.tar.gz \
  | sudo tar -xz --strip-components=1 -C /usr/local/bin --wildcards "*/k6"

# gh CLI (optional, only needed for private repos)
# ... see gh docs

# default GCP user gets docker group
usermod -aG docker $(getent passwd 1001 | cut -d: -f1 || echo edy) 2>/dev/null || true

echo READY > /var/log/rinha-vm.ready
```

After provisioning, poll for ready:

```bash
until gcloud compute ssh rinha-mac-mini-2014 --zone=<zone> \
  --command='test -f /var/log/rinha-vm.ready && echo READY' 2>/dev/null \
  | grep -q READY; do sleep 20; done
```

Verify versions before running anything:

```bash
gcloud compute ssh rinha-mac-mini-2014 --zone=<zone> --command='
  docker --version
  docker compose version
  k6 version
  cat /proc/cpuinfo | grep "model name" | head -1
  free -h | grep Mem
  nproc'
```

CPU should be `Intel(R) Xeon(R) CPU @ 2.30GHz` with `avx2 bmi2 fma`
flags. RAM ~7.7G.

## Cloning the repo onto the VM

For a public repo, plain HTTPS works. For private, the simplest is to
copy your local `gh auth token` once and write it to a credential helper
or use it inline. Don't commit the token. After cloning, the
working tree on the VM is the source of truth for builds.

```bash
gcloud compute ssh rinha-mac-mini-2014 --zone=<zone> --command='
  git clone https://github.com/<user>/<repo>.git $HOME/rinha
  cd $HOME/rinha
  mkdir -p stress/test
  curl -sSL -o stress/test/test.js          https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/main/test/test.js
  curl -sSL -o stress/test/k6-summary.js    https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/main/test/k6-summary.js
  curl -sSL -o stress/test/test-data.json   https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/main/test/test-data.json
  chmod 777 stress/test  # k6 inside its container needs to write results.json'
```

## Running a stress test

`runtest.sh` lives in `$HOME/` on the VM (uploaded from the repo) and
takes a run name + optional extra compose files:

```bash
gcloud compute ssh rinha-mac-mini-2014 --zone=<zone> --command='
  ~/runtest.sh <run-name> [extra-compose.yml ...]'
```

Examples:

```bash
# default stack (Puma)
~/runtest.sh baseline

# Ractor stack
~/runtest.sh ractor-r1 docker-compose.ractor.yml

# override per-run env knobs (Ractor pool size, nprobe)
NPROBE=70 FAST_NPROBE=5 ACCEPTORS=2 ~/runtest.sh tuned-r1 docker-compose.ractor.yml
```

Each invocation does:
1. `docker compose down -v` (clean slate)
2. `docker compose build`
3. `docker compose up -d --wait --wait-timeout 180` (gated by the
   per-container healthcheck — the unix socket file existence)
4. `docker run grafana/k6:latest run /test/test.js` over host network,
   with k6 in its own container at 2 CPU / 8 GB (mirrors the rinha
   harness)
5. `docker compose down -v`

Results go to `~/runs/<run-name>/`:
- `results.json` — the official k6 output
- `docker-stats.log` — per-second `docker stats` snapshots
- `cgroup-poll.log` — `cpu.stat` + `memory.current` sampled every 2 s
- `vmstat.log`, `iostat.log` — system metrics
- `api{1,2}-boot.log`, `api{1,2}-final.log`, `lb-final.log`

A one-liner score check from the host:

```bash
gcloud compute ssh rinha-mac-mini-2014 --zone=<zone> --command='
  python3 -c "import json; d = json.load(open(\"$HOME/runs/<name>/results.json\"));
  s = d[\"scoring\"];
  print(f\"score={s[\"final_score\"]:.1f} p99={d[\"p99\"]} errors={s[\"breakdown\"][\"http_errors\"]}\")"'
```

## Uploading a new ivf.bin.gz (the training-local-then-upload trick)

Re-training k-means with 3 M vectors takes ~3 minutes on an M4 vs
~5 minutes on the VM. Saner to train locally and ship the file. Two
gotchas:

1. **The git working tree on the VM will conflict.** `git pull` refuses
   to rebase over a modified tracked file. The dance:

   ```bash
   gcloud compute ssh rinha-mac-mini-2014 --zone=<zone> --command='
     cd ~/rinha
     cp native/ivf.bin.gz /tmp/local-train.gz   # save what we just scp''d
     git checkout -- native/ivf.bin.gz          # let git pull succeed
     git pull --rebase
     cp /tmp/local-train.gz native/ivf.bin.gz   # restore the local-train file'
   ```

2. **Don't forget to also commit the new ivf.bin.gz upstream** if it's
   going into a published image. Otherwise the GHA publish workflow
   builds from a clean `git clone` that doesn't have your local file,
   and you end up with NEW code reading OLD data — exactly the bug
   that killed submissions #6496 and #6547.

```bash
# from your dev machine, after training:
gcloud compute scp native/ivf.bin.gz rinha-mac-mini-2014:~/rinha/native/ivf.bin.gz --zone=<zone>
# also:
git add native/ivf.bin.gz && git commit -m "..." && git push
```

## Profiling the application

For nginx access log + request-time distribution:

```bash
gcloud compute ssh rinha-mac-mini-2014 --zone=<zone> --command='
  ~/profile_run.sh <name> docker-compose.ractor.yml'
```

Uses `docker-compose.profile.yml` override that mounts a custom
`nginx.profile.conf` (logs `request_time`, `upstream_response_time`,
`upstream_connect_time` per request). The access.log lands in
`~/runs/<name>/access.log`.

Analyze with the helper script (sample, p50/p95/p99, bucketed
histogram):

```bash
python3 /tmp/profile_analyze.py ~/runs/<name>/access.log
```

For per-instruction CPU sampling, rebuild the image with the profile
build args (`PROFILE=1` enables `-g -fno-omit-frame-pointer` in the
extension) and run `perf record`:

```bash
gcloud compute ssh rinha-mac-mini-2014 --zone=<zone> --command='
  ~/perf_run2.sh <name> docker-compose.ractor.yml'
```

Caveats:
- perf needs to be system-wide (`-a`) plus cgroup-filtered (`-G`); see
  the script. `-e cycles --call-graph fp` is the working combo.
- To resolve symbols inside the container's `.so`, copy it out into a
  `symfs/` tree mirroring the in-container path
  (`symbols/app/ext/fraud_index/fraud_index.so`) and pass `--symfs`
  to `perf report`.

## Reading the cgroup data

`cpu.stat` deltas from start to end of a 120-second test give you the
real CPU utilization story:

| field             | derived metric                     | reading |
|-------------------|-------------------------------------|---------|
| `usage_usec`      | CPU time used                       | Δ / 120s ÷ 1 = utilization in CPUs |
| `nr_periods`      | how many 100 ms windows elapsed     | ~1210 for a 120s test |
| `nr_throttled`    | how many of those hit the quota     | ratio is "fraction of windows pinned" |
| `throttled_usec`  | wall time the cgroup sat throttled  | Δ / 120s = fraction of wall time idle waiting |

The "I don't see CPU saturation" trap: `docker stats` shows averaged
CPU% over its 1-second poll window, which looks low (e.g. 20 %)
because the container bursts to 100 % then idles 60 ms each period.
The cgroup stats reveal that 60 % of the 100 ms periods are actually
hitting the ceiling — that's the throttling that builds p99 tail.

## Common debug situations

**Container marked unhealthy at boot:**
1. `docker compose logs api1` (or api2). Look for `ivf.mmap` line —
   if missing, the C++ load failed silently. If present but no
   `warmup:` and no `Listening on`, the warmup crashed.
2. Most common cause we have hit: `ivf.bin.gz` file in the image
   doesn't match the format the loader expects. Either the index
   was retrained but not committed, or the format was changed in
   code without retraining.
3. Verify by extracting the file from the image:
   ```bash
   docker compose run --rm api1 head -c 16 /data/ivf.bin | xxd
   ```
   Compare against the expected header (K, D, N, scale).

**k6 reports "Insufficient VUs, reached 250 active VUs":** the API is
slower than 250 VUs / 900 RPS = 278 ms average response time. Test
still completes but at lower-than-targeted RPS. Real bottleneck is
in the backend — see profiling docs.

**SSH connection reset by peer / 255 exit:** the VM is under heavy CPU
load and sshd starves. Wait, retry. If it persists, the most disruptive
fix is `gcloud compute instances reset <name>`.

**`docker compose up` accepts `cpus:` and `memory:` but nothing happens:**
those live under `deploy:` and only apply in Swarm by default; verify
inside the running container with `cat /sys/fs/cgroup/cpu.max`.
Modern docker-compose (v2.x+) does honor `deploy.resources.limits.cpus`
in non-Swarm mode — but if you see no enforcement, you're on an older
version.

**Local dev (M4 ARM):** k_means builds and runs natively on M4 — the
ivf.bin format is endian-agnostic between ARM64 and x86_64. The
C++ extension on ARM falls back to the scalar dist_sq (no `__AVX2__`),
so smoke tests run but are slower than on the VM. Don't infer
performance from M4 numbers.

## Cleanup

```bash
gcloud compute instances stop  rinha-mac-mini-2014 --zone=<zone>   # keep disk, pay storage only
gcloud compute instances delete rinha-mac-mini-2014 --zone=<zone>  # nuke
```
