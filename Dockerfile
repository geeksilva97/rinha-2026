FROM ruby:4.0.2-slim

ENV BUNDLE_WITHOUT="development:test" \
    BUNDLE_DEPLOYMENT="true" \
    RACK_ENV=production \
    IVF_PATH=/data/ivf.bin

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential \
    && rm -rf /var/lib/apt/lists/*

COPY Gemfile Gemfile.lock ./
RUN bundle install --jobs 4

# Header do IVF index (compartilhado entre native/ standalone e a extension)
COPY native/ivf_index.hpp ./native/ivf_index.hpp

# Compila a C++ extension dentro do build (será incluída na imagem)
COPY ext ./ext
RUN cd ext/fraud_index \
    && ruby extconf.rb \
    && make -j$(nproc) \
    && rm -f *.o   # remove só intermediates, mantém o .so

COPY resources ./resources
COPY app.rb config.ru vectorizer.rb ./
COPY config ./config

CMD ["bundle", "exec", "puma", "-C", "config/puma.rb", "config.ru"]
