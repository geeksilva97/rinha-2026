# Compares three paths from raw body bytes to a fraud score:
#   1) Oj.load + Vectorizer.to_vec + FraudIndex.score(vec)
#   2) FraudIndex.parse_payload(body, buf) + FraudIndex.score(buf)
#   3) FraudIndex.fraud_count_payload(body)   (single C call, returns Integer)
#
# Run with: ruby --yjit -I.. bench/cpp_payload_bench.rb

require 'benchmark/ips'
require 'oj'
$LOAD_PATH.unshift(File.expand_path('../ext/fraud_index', __dir__))
require 'fraud_index'
require_relative '../vectorizer'

FraudIndex.load(File.expand_path('../native/ivf.bin', __dir__))
FraudIndex.nprobe = 70

PAYLOADS = Oj.load(File.read(File.expand_path('../resources/example-payloads.json', __dir__)))
BODIES   = PAYLOADS.map { |p| Oj.dump(p) }.freeze

body = BODIES[0]
p0   = PAYLOADS[0]
buf  = Array.new(14, 0.0)

# Sanity: all three should produce equivalent results for every payload.
PAYLOADS.each_with_index do |p, i|
  b = BODIES[i]
  s1 = FraudIndex.score(Vectorizer.to_vec(p, buf))
  v2 = FraudIndex.parse_payload(b, Array.new(14, 0.0))
  s2 = FraudIndex.score(v2)
  s3 = FraudIndex.fraud_count_payload(b) / 5.0
  unless (s1 - s2).abs < 1e-6 && (s1 - s3).abs < 1e-6
    warn "MISMATCH at #{i}: oj=#{s1} parse=#{s2} payload=#{s3}"
    exit 1
  end
end
puts "✓ all three paths agree for #{PAYLOADS.size} payloads"
puts "ruby: #{RUBY_VERSION}  yjit: #{defined?(RubyVM::YJIT) && RubyVM::YJIT.enabled? ? 'ON' : 'off'}"
puts

Benchmark.ips do |x|
  x.config(time: 5, warmup: 2)
  x.report('oj+vec+score')           { FraudIndex.score(Vectorizer.to_vec(Oj.load(body), buf)) }
  x.report('parse_payload+score')    { v = FraudIndex.parse_payload(body, buf); FraudIndex.score(v) }
  x.report('fraud_count_payload')    { FraudIndex.fraud_count_payload(body) }
  x.compare!
end
