FROM --platform=linux/amd64 ruby:4.0.2-slim

ARG CXX_MARCH=haswell
ENV BUNDLE_WITHOUT="development:test" \
    BUNDLE_DEPLOYMENT="true" \
    RACK_ENV=production \
    IVF_PATH=/data/ivf.bin \
    CXX_MARCH=${CXX_MARCH}

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential \
    && rm -rf /var/lib/apt/lists/*

COPY Gemfile Gemfile.lock ./
RUN bundle install --jobs 4

# IVF index header (shared between native/ standalone and the extension)
COPY native/ivf_index.hpp ./native/ivf_index.hpp

# Build the C++ extension during image build (shipped in the image)
COPY ext ./ext
RUN cd ext/fraud_index \
    && ruby extconf.rb \
    && make -j$(nproc) \
    && rm -f *.o   # drop only intermediates, keep the .so

# Ship the trained IVF index inside the image.
# Committed as gzip (~42MB, fits GitHub); decompressed here at build time.
COPY native/ivf.bin.gz /data/ivf.bin.gz
RUN gunzip /data/ivf.bin.gz \
    && ls -lh /data/ivf.bin

COPY resources ./resources
COPY app.rb config.ru vectorizer.rb server_ractor.rb ./
COPY config ./config

# Default entrypoint is Puma. The Ractor-per-request server is also
# shipped (server_ractor.rb) — submission can override CMD to use it.
CMD ["bundle", "exec", "puma", "-C", "config/puma.rb", "config.ru"]
