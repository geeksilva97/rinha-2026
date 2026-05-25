# Minimal Ractor-per-request HTTP server (experimental).
#
# main thread: accept(); spawn a Ractor with the FD. Ractor handles the
# full request lifecycle (read, parse, FraudIndex call, write, close) and
# dies. No IPC for the data path.

$LOAD_PATH.unshift('/app/ext/fraud_index')
require 'fraud_index'
require 'socket'
require 'oj'

idx_path = ENV.fetch('IVF_PATH', '/data/ivf.bin')
FraudIndex.load(idx_path) or abort "FraudIndex.load failed for #{idx_path}"
FraudIndex.nprobe = Integer(ENV.fetch('NPROBE', '70'))
warn "FraudIndex loaded (nprobe=#{FraudIndex.nprobe})"

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

sock_path = ENV.fetch('SOCK')
File.unlink(sock_path) if File.exist?(sock_path)
server = UNIXServer.new(sock_path)
File.chmod(0o666, sock_path)
warn "Listening on unix://#{sock_path}"

# Multiple acceptor threads parallelize the accept+Ractor.new path. With
# 1 thread, a Ractor.new (~150µs) blocks all incoming connections behind
# it. With N threads, accept can be in flight on N-1 while one is spawning.
ACCEPTORS = Integer(ENV.fetch('ACCEPTORS', '2'))
warn "Acceptors: #{ACCEPTORS}"

threads = ACCEPTORS.times.map do
  Thread.new do
    loop do
      client = server.accept
      fd = client.fileno
      client.autoclose = false   # ownership transferred to the Ractor

      Ractor.new(fd) do |fd|
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
  end
end

threads.each(&:join)
