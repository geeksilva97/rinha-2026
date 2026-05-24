# Ractor dispatch overhead microbench — Ruby 4.0 port API.
#
# 1) direct      — N sequential FraudIndex calls in main Ractor
# 2) ractor-K    — K worker Ractors sharing one reply Port; round-robin dispatch

$LOAD_PATH.unshift('/app/ext/fraud_index') if File.exist?('/app/ext/fraud_index/fraud_index.so')
$LOAD_PATH.unshift(File.expand_path('../ext/fraud_index', __dir__))
require 'fraud_index'

idx_path = ENV.fetch('IVF_PATH', File.exist?('/data/ivf.bin') ? '/data/ivf.bin' : File.expand_path('../native/ivf.bin', __dir__))
FraudIndex.load(idx_path) or abort "load failed"
FraudIndex.nprobe = Integer(ENV.fetch('NPROBE', '70'))
warn "FraudIndex loaded (nprobe=#{FraudIndex.nprobe})"

require 'json'
example = ['/app/resources/example-payloads.json', File.expand_path('../resources/example-payloads.json', __dir__)].find { |p| File.exist?(p) } or abort "example-payloads.json not found"
bodies = JSON.parse(File.read(example)).map { |h| JSON.generate(h).freeze }
warn "#{bodies.size} bodies"

N = Integer(ENV.fetch('N', '20000'))
clk = Process::CLOCK_MONOTONIC

# warm
1000.times { |i| FraudIndex.fraud_count_payload(bodies[i % bodies.size]) }

t0 = Process.clock_gettime(clk, :nanosecond)
N.times { |i| FraudIndex.fraud_count_payload(bodies[i % bodies.size]) }
direct_ns = Process.clock_gettime(clk, :nanosecond) - t0

def run_ractors(k, bodies, n)
  reply = Ractor::Port.new
  workers = Array.new(k) do
    Ractor.new(reply) do |out|
      loop do
        body = Ractor.receive
        break if body == :stop
        out << FraudIndex.fraud_count_payload(body)
      end
    end
  end
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond)
  n.times { |i| workers[i % k] << bodies[i % bodies.size] }
  n.times { reply.receive }
  ns = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond) - t0
  workers.each { |w| w << :stop }
  ns
end

results = { direct: direct_ns }
[1, 2, 4].each { |k| results[k] = run_ractors(k, bodies, N) }

puts
printf "%-12s %10s %10s\n", "mode", "total(ms)", "us/req"
puts "-" * 36
results.each do |k, ns|
  label = k == :direct ? "direct" : "ractor-#{k}"
  printf "%-12s %10.2f %10.2f\n", label, ns / 1e6, ns / N / 1e3
end
