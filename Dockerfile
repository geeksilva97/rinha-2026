FROM ruby:4.0.2-slim

ENV BUNDLE_WITHOUT="development:test" \
    BUNDLE_DEPLOYMENT="true" \
    RACK_ENV=production

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential \
    && rm -rf /var/lib/apt/lists/*

COPY Gemfile Gemfile.lock ./
RUN bundle install --jobs 4

COPY app.rb config.ru ./
COPY config ./config

CMD ["bundle", "exec", "puma", "-C", "config/puma.rb", "config.ru"]
