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

# Header do IVF index (compartilhado entre native/ standalone e a extension)
COPY native/ivf_index.hpp ./native/ivf_index.hpp

# Build the C++ extension during image build (shipped in the image)
COPY ext ./ext
RUN cd ext/fraud_index \
    && ruby extconf.rb \
    && make -j$(nproc) \
    && rm -f *.o   # drop only intermediates, keep the .so

COPY resources ./resources
COPY app.rb config.ru vectorizer.rb ./
COPY config ./config

CMD ["bundle", "exec", "puma", "-C", "config/puma.rb", "config.ru"]
