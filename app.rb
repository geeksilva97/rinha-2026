require 'oj'

$LOAD_PATH.unshift(File.expand_path('ext/fraud_index', __dir__))
require 'fraud_index'
require_relative 'vectorizer'

Oj.default_options = { mode: :strict, symbol_keys: false }

# Carrega o índice IVF no boot. Path via env (Docker mountará /data/ivf.bin).
IVF_PATH = ENV.fetch('IVF_PATH', File.expand_path('native/ivf.bin', __dir__))
unless FraudIndex.load(IVF_PATH)
  raise "FraudIndex.load failed for #{IVF_PATH}"
end
warn "FraudIndex loaded from #{IVF_PATH}"

class App
  READY_RESPONSE = [200, { 'content-type' => 'text/plain' }.freeze, ['ok'.freeze]].freeze
  NOT_FOUND      = [404, { 'content-type' => 'text/plain' }.freeze, ['not found'.freeze]].freeze
  BAD_REQUEST    = [400, { 'content-type' => 'text/plain' }.freeze, ['bad request'.freeze]].freeze
  JSON_HEADERS   = { 'content-type' => 'application/json' }.freeze
  THRESHOLD      = 0.6

  def initialize
    # Buffer reutilizado pelo Vectorizer pra evitar alocar Array(14) por request.
    # Puma com workers=1, threads=1 → single-threaded por processo, safe reusar.
    @vec_buf = Array.new(14, 0.0)
  end

  def call(env)
    case env['REQUEST_METHOD']
    when 'GET'
      return READY_RESPONSE if env['PATH_INFO'] == '/ready'
    when 'POST'
      return fraud_score(env) if env['PATH_INFO'] == '/fraud-score'
    end

    NOT_FOUND
  end

  private

  def fraud_score(env)
    payload = Oj.load(env['rack.input'].read)
    vec     = Vectorizer.to_vec(payload, @vec_buf)
    score   = FraudIndex.score(vec)

    response = { 'approved' => score < THRESHOLD, 'fraud_score' => score }
    [200, JSON_HEADERS, [Oj.dump(response)]]
  rescue Oj::ParseError
    BAD_REQUEST
  end
end
