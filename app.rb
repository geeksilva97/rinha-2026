require 'oj'

$LOAD_PATH.unshift(File.expand_path('ext/fraud_index', __dir__))
require 'fraud_index'
require_relative 'vectorizer'

Oj.default_options = { mode: :strict, symbol_keys: false }

# Load the IVF index at boot. Path via env (Docker mounts /data/ivf.bin).
IVF_PATH = ENV.fetch('IVF_PATH', File.expand_path('native/ivf.bin', __dir__))
unless FraudIndex.load(IVF_PATH)
  raise "FraudIndex.load failed for #{IVF_PATH}"
end
FraudIndex.nprobe = Integer(ENV.fetch('NPROBE', '1'))
warn "FraudIndex loaded from #{IVF_PATH} (nprobe=#{FraudIndex.nprobe})"

# Warmup with realistic payloads so the first real requests don't pay for
# cold page cache, cold branch predictors, or cold method caches. Uses the
# example-payloads (same statistical distribution as test-data) — the
# warmup queries hit roughly the same clusters as real queries will.
# Tunable via WARMUP env (rounds over the example set). 0 disables.
WARMUP_ROUNDS = Integer(ENV.fetch('WARMUP', '10'))
warmup_path = File.expand_path('resources/example-payloads.json', __dir__)
if WARMUP_ROUNDS > 0 && File.exist?(warmup_path)
  warmup_bodies = Oj.load(File.read(warmup_path)).map { |p| Oj.dump(p) }
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC, :millisecond)
  WARMUP_ROUNDS.times do
    warmup_bodies.each { |b| FraudIndex.fraud_count_payload(b) }
  end
  elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC, :millisecond) - t0
  warn "warmup: #{WARMUP_ROUNDS}x#{warmup_bodies.size}=#{WARMUP_ROUNDS * warmup_bodies.size} queries in #{elapsed}ms"
end

# Per-stage timing instrumentation. Toggle via env INSTRUMENT=1.
# Adds ~6 calls to Process.clock_gettime per request (~50ns each = ~300ns total).
INSTRUMENT = ENV['INSTRUMENT'] == '1'

class App
  READY_RESPONSE = [200, { 'content-type' => 'text/plain' }.freeze, ['ok'.freeze]].freeze
  NOT_FOUND      = [404, { 'content-type' => 'text/plain' }.freeze, ['not found'.freeze]].freeze
  BAD_REQUEST    = [400, { 'content-type' => 'text/plain' }.freeze, ['bad request'.freeze]].freeze
  JSON_HEADERS   = { 'content-type' => 'application/json' }.freeze
  THRESHOLD      = 0.6

  # Pre-computed responses, one per possible fraud_count (0..5).
  # threshold=0.6, so 0/5, 1/5, 2/5 → approved=true; 3/5, 4/5, 5/5 → false.
  RESPONSES = [
    '{"approved":true,"fraud_score":0.0}'.freeze,
    '{"approved":true,"fraud_score":0.2}'.freeze,
    '{"approved":true,"fraud_score":0.4}'.freeze,
    '{"approved":false,"fraud_score":0.6}'.freeze,
    '{"approved":false,"fraud_score":0.8}'.freeze,
    '{"approved":false,"fraud_score":1.0}'.freeze,
  ].freeze
  # Cache the wrapped rack triplets too, so the hot path returns a frozen
  # array literal instead of building a fresh one.
  RESPONSE_TUPLES = RESPONSES.map { |body| [200, JSON_HEADERS, [body]].freeze }.freeze

  STAGES = %i[read parse vec score dump total].freeze
  STATS = Hash.new(0)
  STATS_MUTEX = Mutex.new

  def initialize
    @vec_buf = Array.new(14, 0.0)
  end

  def call(env)
    case env['REQUEST_METHOD']
    when 'GET'
      return READY_RESPONSE if env['PATH_INFO'] == '/ready'
      return stats_response  if env['PATH_INFO'] == '/stats'
      return reset_stats     if env['PATH_INFO'] == '/stats/reset'
    when 'POST'
      return fraud_score(env) if env['PATH_INFO'] == '/fraud-score'
    end
    NOT_FOUND
  end

  private

  def fraud_score(env)
    return fraud_score_instrumented(env) if INSTRUMENT

    # Single C call: parse the body bytes + run kNN + return Integer 0..5.
    # No Hash, no Float allocation, no Oj.dump on the response — just an
    # Integer index into a frozen Array of pre-built rack tuples.
    RESPONSE_TUPLES[FraudIndex.fraud_count_payload(env['rack.input'].read)]
  end

  def fraud_score_instrumented(env)
    clk = Process::CLOCK_MONOTONIC
    t0 = Process.clock_gettime(clk, :nanosecond)
    raw = env['rack.input'].read
    t1 = Process.clock_gettime(clk, :nanosecond)
    count = FraudIndex.fraud_count_payload(raw)
    t2 = Process.clock_gettime(clk, :nanosecond)
    response = RESPONSE_TUPLES[count]
    t3 = Process.clock_gettime(clk, :nanosecond)

    STATS_MUTEX.synchronize do
      STATS[:count]            += 1
      STATS[:read_ns]          += (t1 - t0)
      STATS[:parse_score_ns]   += (t2 - t1)
      STATS[:lookup_ns]        += (t3 - t2)
      STATS[:total_ns]         += (t3 - t0)
    end

    response
  end

  def stats_response
    snap = STATS_MUTEX.synchronize { STATS.dup }
    n = snap[:count]
    if n.zero?
      return [200, JSON_HEADERS, [Oj.dump({ 'count' => 0 })]]
    end
    body = {
      'count'   => n,
      'avg_ns'  => {
        'read'  => snap[:read_ns]  / n,
        'parse' => snap[:parse_ns] / n,
        'vec'   => snap[:vec_ns]   / n,
        'score' => snap[:score_ns] / n,
        'dump'  => snap[:dump_ns]  / n,
        'total' => snap[:total_ns] / n
      },
      'pct_of_total' => {
        'read'  => (100.0 * snap[:read_ns]  / snap[:total_ns]).round(1),
        'parse' => (100.0 * snap[:parse_ns] / snap[:total_ns]).round(1),
        'vec'   => (100.0 * snap[:vec_ns]   / snap[:total_ns]).round(1),
        'score' => (100.0 * snap[:score_ns] / snap[:total_ns]).round(1),
        'dump'  => (100.0 * snap[:dump_ns]  / snap[:total_ns]).round(1)
      }
    }
    [200, JSON_HEADERS, [Oj.dump(body)]]
  end

  def reset_stats
    STATS_MUTEX.synchronize { STATS.clear }
    [200, JSON_HEADERS, ['{"reset":true}']]
  end
end
