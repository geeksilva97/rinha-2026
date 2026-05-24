# rinha-fraud-detection — submission

Submission branch for [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026).

Source code lives on the `main` branch:
[geeksilva97/rinha-2026](https://github.com/geeksilva97/rinha-2026).

## Stack

- Ruby 4.0.2 (Puma + Rack)
- C++ extension for the hot path (IVF kNN search, AVX2/NEON SIMD)
- nginx as load balancer
- Trained IVF index baked into the image (no external storage)

## Files

- `docker-compose.yml` — three-service stack (api1, api2, lb)
- `nginx.conf` — load balancer routing to api1/api2 via unix sockets
- `info.json` — submission metadata
- `LICENSE` — MIT

## Image

Pre-built and pinned by digest from `ghcr.io/geeksilva97/rinha-2026-api`.
