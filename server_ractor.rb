# Ractor-pool HTTP server.
#
# Pre-spawns ACCEPTORS worker Ractors at boot. Each worker loops on
# `Ractor.receive` waiting for an FD; the main thread accepts and
# dispatches via round-robin. Eliminates the ~150µs Ractor.new cost
# per request that the spawn-per-request design paid.

$LOAD_PATH.unshift('/app/ext/fraud_index')
require 'fraud_index'
require 'socket'
require 'oj'

idx_path = ENV.fetch('IVF_PATH', '/data/ivf.bin')
FraudIndex.load(idx_path) or abort "FraudIndex.load failed for #{idx_path}"
FraudIndex.nprobe = Integer(ENV.fetch('NPROBE', '70'))
FraudIndex.fast_nprobe = Integer(ENV.fetch('FAST_NPROBE', '5'))
warn "FraudIndex loaded (nprobe=#{FraudIndex.nprobe} fast_nprobe=#{FraudIndex.fast_nprobe})"

WARMUP_ROUNDS = Integer(ENV.fetch('WARMUP', '10'))
warmup_path = '/app/resources/example-payloads.json'
if WARMUP_ROUNDS > 0 && File.exist?(warmup_path)
  Oj.default_options = { mode: :strict }
  bodies = Oj.load(File.read(warmup_path)).map { |p| Oj.dump(p) }
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC, :millisecond)
  WARMUP_ROUNDS.times { bodies.each { |b| FraudIndex.fraud_count_payload(b) } }
  ms = Process.clock_gettime(Process::CLOCK_MONOTONIC, :millisecond) - t0
  warn "warmup: #{WARMUP_ROUNDS}x#{bodies.size}=#{WARMUP_ROUNDS * bodies.size} queries in #{ms}ms"
end

# Pre-built HTTP responses (frozen → Ractor-shareable).
def build_response(approved, score)
  body = %Q({"approved":#{approved},"fraud_score":#{score}})
  "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: #{body.bytesize}\r\nConnection: close\r\n\r\n#{body}".freeze
end
RESPONSES = Ractor.make_shareable([
  build_response(true,  0.0),
  build_response(true,  0.2),
  build_response(true,  0.4),
  build_response(false, 0.6),
  build_response(false, 0.8),
  build_response(false, 1.0),
])
READY_RESPONSE = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok".freeze
NOT_FOUND      = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".freeze
BAD_REQUEST    = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".freeze

# Worker Ractor body: loops on receive(fd), handles one request per loop,
# never exits. Same per-request logic as the spawn-per-request version,
# but the VM/heap/branch-predictor state stays hot across requests.
WORKER_BODY = proc do
  loop do
    fd = Ractor.receive
    break if fd == :stop

    io = IO.for_fd(fd, "r+", autoclose: true)
    response = nil
    begin
      raw = io.readpartial(8192)
      eol = raw.index("\r\n")
      if eol
        method, path, _ = raw.byteslice(0, eol).split(" ", 3)
        if method == "GET" && path == "/ready"
          response = READY_RESPONSE
        elsif method == "POST" && path == "/fraud-score"
          head_end = raw.index("\r\n\r\n")
          if head_end
            cl_match = raw.byteslice(0, head_end).match(/^Content-Length:\s*(\d+)/i)
            cl = cl_match ? cl_match[1].to_i : 0
            body_start = head_end + 4
            body = raw.byteslice(body_start, raw.bytesize - body_start) || +""
            while body.bytesize < cl
              chunk = io.readpartial(cl - body.bytesize)
              break unless chunk
              body << chunk
            end
            count = FraudIndex.fraud_count_payload(body)
            response = RESPONSES[count]
          else
            response = BAD_REQUEST
          end
        else
          response = NOT_FOUND
        end
      else
        response = BAD_REQUEST
      end
    rescue
      response = BAD_REQUEST
    end
    begin
      io.write(response) if response
    rescue
      # client gone
    ensure
      io.close rescue nil
    end
  end
end

sock_path = ENV.fetch('SOCK')
File.unlink(sock_path) if File.exist?(sock_path)
server = UNIXServer.new(sock_path)
# Bump listen() backlog — UNIXServer.new defaults to SOMAXCONN or 128
# depending on libc. Under 900 RPS bursts, a small backlog can silently
# drop connections in the kernel before we accept them. Cap requested
# at 1024 (kernel enforces min(this, /proc/sys/net/core/somaxconn)).
server.listen(1024)
File.chmod(0o666, sock_path)
warn "Listening on unix://#{sock_path} (backlog=1024)"

# Pool size (worker Ractors) — env ACCEPTORS, default 2.
ACCEPTORS = Integer(ENV.fetch('ACCEPTORS', '2'))
# Number of accept threads dispatching to the pool — env ACCEPT_THREADS,
# default 1. Each thread keeps its own local counter for round-robin
# (no shared mutex; distribution is best-effort but workers stay full).
ACCEPT_THREADS = Integer(ENV.fetch('ACCEPT_THREADS', '1'))
warn "Worker pool size: #{ACCEPTORS}, accept threads: #{ACCEPT_THREADS}"

workers = ACCEPTORS.times.map { Ractor.new(&WORKER_BODY) }

threads = ACCEPT_THREADS.times.map do |tid|
  Thread.new do
    i = tid   # offset start so multiple accept threads don't all pick worker 0 first
    loop do
      client = server.accept
      fd = client.fileno
      client.autoclose = false     # ownership transfers to the worker Ractor
      workers[i % ACCEPTORS].send(fd)
      i += 1
    end
  end
end

threads.each(&:join)
