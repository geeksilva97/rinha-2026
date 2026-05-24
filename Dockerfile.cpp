# Standalone C++ HTTP server image — drop-in replacement for the Ruby+Puma
# stack. Same SOCK / IVF_PATH / NPROBE env contract as the Ruby Dockerfile.
#
# Build: docker build -f Dockerfile.cpp -t rinha-fraud-detection-api:cpp .
# Run:   docker run --rm -e SOCK=/tmp/api.sock -e IVF_PATH=/data/ivf.bin \
#                  -e NPROBE=70 rinha-fraud-detection-api:cpp

# ─── builder ────────────────────────────────────────────────────────────────
FROM --platform=linux/amd64 gcc:14-bookworm AS builder

ARG CXX_MARCH=haswell
ENV DEBIAN_FRONTEND=noninteractive

# gcc:14 image already ships g++-14 as /usr/local/bin/g++. Just need make.
RUN apt-get update \
    && apt-get install -y --no-install-recommends make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY native/ ./native/

# Override the Makefile's -march=native with the explicit microarch — the
# image will run on machines other than the build host.
RUN cd native \
    && rm -rf _compat \
    && sed -i 's/-march=native/-march='"$CXX_MARCH"'/g' Makefile \
    && make bin/server CXX=g++ \
    && ls -lh bin/server

# IVF index: ship the same gzipped blob already in the repo, gunzip at build.
RUN gunzip -k -c /build/native/ivf.bin.gz > /data_ivf.bin

# ─── runtime ────────────────────────────────────────────────────────────────
FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /build/native/bin/server /app/bin/server
COPY --from=builder /data_ivf.bin /data/ivf.bin
COPY resources /app/resources

ENV IVF_PATH=/data/ivf.bin \
    NPROBE=1 \
    WARMUP=10

# SOCK must be supplied at runtime (compose sets it per replica).
CMD ["/app/bin/server"]
