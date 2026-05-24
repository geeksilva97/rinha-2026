#!/usr/bin/env bash
# Local experiment harness — k6 in a container (matches rinha test setup:
# separate container, host network, 2 CPU / 8G memory).
# Usage: ./lab.sh <name> [env=val ...]
# Example: ./lab.sh threads-2 RACK_MAX_THREADS=2

set -euo pipefail

name="$1"; shift || true

mkdir -p lab/results stress/test/test

out="lab/results/${name}.json"

echo ">>> [$name] tearing down stack + wiping caches"
docker compose down -v 2>&1 | tail -2

echo ">>> [$name] starting api stack with overrides: $* (COMPOSE_FILE=${COMPOSE_FILE:-docker-compose.yml})"
env CXX_MARCH=x86-64 "$@" docker compose up -d --wait --wait-timeout 180 2>&1 | tail -3

echo ">>> [$name] running k6 in container (rinha-style: host network, 2 CPU / 8G)"
docker run --rm \
  --network=host \
  --cpus=2 \
  --memory=8g \
  -v "$PWD/stress/test:/test" \
  -w /test \
  -e K6_NO_USAGE_REPORT=true \
  grafana/k6:latest run --quiet /test/test.js 2>&1 | tail -3

if [ -f stress/test/test/results.json ]; then
  cp stress/test/test/results.json "$out"
  python3 -c "
import json
d = json.load(open('$out'))
s = d['scoring']
print(f\"  -> score={s['final_score']:.1f}  p99={d['p99']}  errors={s['breakdown']['http_errors']}  failure={s['failure_rate']}\")
"
else
  echo "  -> no results.json produced"
fi
