# syntax=docker/dockerfile:1.6
ARG RUBY_VERSION=4.0.2

# ---------- builder ----------
# Compiles its own modern SQLite (Debian Bookworm ships 3.40.1, but vec1's
# iVersion=4 vtab module with xIntegrity requires SQLite >= 3.44).
# Both vec1.so and the sqlite3 gem link against this freshly-built libsqlite3.
FROM ruby:${RUBY_VERSION}-slim-bookworm AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential curl ca-certificates unzip pkg-config \
    && rm -rf /var/lib/apt/lists/*

ARG SQLITE_YEAR=2025
ARG SQLITE_VERSION=3500400
RUN curl -fsSL "https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_VERSION}.zip" -o /tmp/sqlite.zip \
 && unzip -q /tmp/sqlite.zip -d /tmp \
 && cd /tmp/sqlite-amalgamation-${SQLITE_VERSION} \
 && gcc -O2 -fPIC -shared \
      -DSQLITE_ENABLE_LOAD_EXTENSION=1 \
      -DSQLITE_ENABLE_FTS5=1 \
      -DSQLITE_ENABLE_DBSTAT_VTAB=1 \
      -DSQLITE_THREADSAFE=1 \
      sqlite3.c -ldl -lpthread -lm \
      -o /usr/local/lib/libsqlite3.so.0 \
 && ln -sf /usr/local/lib/libsqlite3.so.0 /usr/local/lib/libsqlite3.so \
 && cp /tmp/sqlite-amalgamation-${SQLITE_VERSION}/sqlite3.h     /usr/local/include/ \
 && cp /tmp/sqlite-amalgamation-${SQLITE_VERSION}/sqlite3ext.h  /usr/local/include/ \
 && ldconfig

# Build vec1.so against the freshly-built SQLite headers.
# VEC1_CFLAGS default is "-mavx2 -mfma" for real x86_64 hardware (Rinha runners).
# For local testing on Apple Silicon (Docker buildx --platform linux/amd64 via
# QEMU, which doesn't emulate AVX2) override with: --build-arg VEC1_CFLAGS=""
ARG VEC1_CFLAGS="-mavx2 -mfma"
WORKDIR /build
COPY vendor/vec1/vec1.c ./
RUN gcc -O3 -DNDEBUG ${VEC1_CFLAGS} -I/usr/local/include vec1.c -shared -fPIC -o vec1.so

# Build sqlite3 gem against the same freshly-built SQLite.
ENV BUNDLE_PATH=/usr/local/bundle \
    BUNDLE_WITHOUT=development:test \
    BUNDLE_JOBS=4
WORKDIR /app
COPY Gemfile Gemfile.lock ./
RUN bundle config set --local build.sqlite3 "--enable-system-libraries --with-sqlite3-include=/usr/local/include --with-sqlite3-lib=/usr/local/lib" \
 && bundle install \
 && rm -rf /usr/local/bundle/cache

# ---------- runtime ----------
FROM ruby:${RUBY_VERSION}-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
      libjemalloc2 \
    && rm -rf /var/lib/apt/lists/*

# Copy the freshly-built libsqlite3 into the runtime (must match what the
# sqlite3 gem and vec1.so were linked against in the builder stage).
COPY --from=builder /usr/local/lib/libsqlite3.so.0 /usr/local/lib/
RUN ln -sf /usr/local/lib/libsqlite3.so.0 /usr/local/lib/libsqlite3.so && ldconfig

ENV BUNDLE_PATH=/usr/local/bundle \
    BUNDLE_WITHOUT=development:test \
    LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
    MALLOC_CONF=narenas:2,dirty_decay_ms:1000,muzzy_decay_ms:0 \
    VEC1_EXT=./vendor/vec1/vec1.so

WORKDIR /app
COPY --from=builder /usr/local/bundle /usr/local/bundle
COPY --from=builder /build/vec1.so /app/vendor/vec1/vec1.so

COPY Gemfile Gemfile.lock puma.rb config.ru ./
COPY lib ./lib
COPY resources/mcc_risk.json resources/normalization.json ./resources/
COPY fraud.db ./fraud.db

EXPOSE 9999
CMD ["bundle", "exec", "puma", "-C", "puma.rb"]
