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

# Warmup with dummy queries so the first real requests don't pay for:
#  - cold page cache (only loads pages real queries touch — no cgroup pressure)
#  - cold branch predictor in the SIMD distance loops
#  - cold Ruby method caches / first-time C-extension entry paths
# Tunable via WARMUP=0 to disable, or WARMUP=N to set iterations.
WARMUP_ITERS = Integer(ENV.fetch('WARMUP', '500'))
if WARMUP_ITERS > 0
  rng = Random.new(42)
  buf = Array.new(14, 0.0)
  t_start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :millisecond)
  WARMUP_ITERS.times do
    14.times { |i| buf[i] = rng.rand(-1.0..1.0) }
    FraudIndex.score(buf)
  end
  elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC, :millisecond) - t_start
  warn "warmup: #{WARMUP_ITERS} queries in #{elapsed}ms"
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

    payload = Oj.load(env['rack.input'].read)
    vec     = Vectorizer.to_vec(payload, @vec_buf)
    score   = FraudIndex.score(vec)
    [200, JSON_HEADERS, [Oj.dump({ 'approved' => score < THRESHOLD, 'fraud_score' => score })]]
  rescue Oj::ParseError
    BAD_REQUEST
  end

  def fraud_score_instrumented(env)
    clk = Process::CLOCK_MONOTONIC
    t0 = Process.clock_gettime(clk, :nanosecond)
    raw = env['rack.input'].read
    t1 = Process.clock_gettime(clk, :nanosecond)
    payload = Oj.load(raw)
    t2 = Process.clock_gettime(clk, :nanosecond)
    vec = Vectorizer.to_vec(payload, @vec_buf)
    t3 = Process.clock_gettime(clk, :nanosecond)
    score = FraudIndex.score(vec)
    t4 = Process.clock_gettime(clk, :nanosecond)
    body = Oj.dump({ 'approved' => score < THRESHOLD, 'fraud_score' => score })
    t5 = Process.clock_gettime(clk, :nanosecond)

    STATS_MUTEX.synchronize do
      STATS[:count]    += 1
      STATS[:read_ns]  += (t1 - t0)
      STATS[:parse_ns] += (t2 - t1)
      STATS[:vec_ns]   += (t3 - t2)
      STATS[:score_ns] += (t4 - t3)
      STATS[:dump_ns]  += (t5 - t4)
      STATS[:total_ns] += (t5 - t0)
    end

    [200, JSON_HEADERS, [body]]
  rescue Oj::ParseError
    BAD_REQUEST
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
