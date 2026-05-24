// Standalone C++ HTTP/1.1 server that replaces Puma+Ruby in the
// rinha-fraud-detection stack. Single binary, listens on a unix socket,
// keep-alive, single-threaded event loop with blocking accept/read/write.
//
// Endpoints:
//   GET  /ready        → 200 "ok"
//   POST /fraud-score  → 200 JSON, one of 6 pre-computed responses
//
// Env:
//   SOCK     (required) unix socket path
//   IVF_PATH (default /data/ivf.bin)
//   NPROBE   (default 1)
//   WARMUP   (default 10) realistic-payload rounds before serving traffic
//
// Behavior matches app.rb byte-for-byte on /fraud-score (same response
// strings indexed by fraud_count 0..5).

#include "fraud_payload.hpp"
#include "ivf_index.hpp"
#include "vendor/picohttpparser/picohttpparser.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace ivf;

// ─── globals + signal handling ───────────────────────────────────────────────
static std::atomic<int> g_listen_fd{-1};
static std::string      g_sock_path;
static std::atomic<bool> g_shutdown{false};

static void on_signal(int /*sig*/) {
  g_shutdown.store(true);
  int fd = g_listen_fd.exchange(-1);
  if (fd >= 0) ::close(fd);
}

static void cleanup_socket() {
  if (!g_sock_path.empty()) ::unlink(g_sock_path.c_str());
}

// ─── pre-computed responses ──────────────────────────────────────────────────
// One pre-built buffer per fraud_count value 0..5. Each contains the full
// HTTP/1.1 response: status line, headers (incl. Content-Length and
// Connection: keep-alive), CRLFCRLF, then the JSON body. Written verbatim.

static const char *kReadyResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "ok";

static const char *kNotFoundResponse =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 9\r\n"
    "Connection: close\r\n"
    "\r\n"
    "not found";

static const char *kBadRequestResponse =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n"
    "\r\n"
    "bad request";

// Bodies must match Ruby's RESPONSES array exactly.
static const char *const kFraudBodies[6] = {
    "{\"approved\":true,\"fraud_score\":0.0}",
    "{\"approved\":true,\"fraud_score\":0.2}",
    "{\"approved\":true,\"fraud_score\":0.4}",
    "{\"approved\":false,\"fraud_score\":0.6}",
    "{\"approved\":false,\"fraud_score\":0.8}",
    "{\"approved\":false,\"fraud_score\":1.0}",
};

struct Prebuilt {
  std::string buf;  // full HTTP response (headers + body)
};

static Prebuilt kFraudResponses[6];

static void build_fraud_responses() {
  for (int i = 0; i < 6; ++i) {
    const char *body = kFraudBodies[i];
    size_t blen = std::strlen(body);
    std::ostringstream os;
    os << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << blen << "\r\n"
       << "Connection: close\r\n"
       << "\r\n"
       << body;
    kFraudResponses[i].buf = os.str();
  }
}

// ─── write helper ────────────────────────────────────────────────────────────
static bool write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::write(fd, buf + off, len - off);
    if (n > 0) { off += static_cast<size_t>(n); continue; }
    if (n < 0 && (errno == EINTR)) continue;
    return false;
  }
  return true;
}

// ─── connection handling ─────────────────────────────────────────────────────
// Per-connection scratch buffer. Single-threaded server → one global is fine.
constexpr size_t kBufSize = 32 * 1024;  // 32KB
constexpr size_t kMaxHeaders = 32;

struct ConnState {
  std::vector<char> buf;
  size_t            filled = 0;
  ConnState() : buf(kBufSize) {}
};

// Try to parse one full request out of `buf[0..filled)`. Returns the number
// of bytes consumed if a full request was processed, 0 if more data is
// needed, -1 on protocol error.
static int handle_one_request(int cfd, ConnState &st, const IvfIndex &idx,
                              int nprobe, bool &keep_alive) {
  const char *method = nullptr;
  size_t method_len = 0;
  const char *path = nullptr;
  size_t path_len = 0;
  int minor_version = 0;
  struct phr_header headers[kMaxHeaders];
  size_t num_headers = kMaxHeaders;

  int pret = phr_parse_request(st.buf.data(), st.filled,
                               &method, &method_len, &path, &path_len,
                               &minor_version, headers, &num_headers, 0);
  if (pret == -2) return 0;            // need more data
  if (pret < 0) return -1;             // malformed
  size_t header_len = static_cast<size_t>(pret);

  // Find content-length and connection headers.
  size_t content_length = 0;
  bool   close_after = (minor_version == 0); // HTTP/1.0 default close
  for (size_t i = 0; i < num_headers; ++i) {
    const char *n = headers[i].name;
    size_t nl = headers[i].name_len;
    const char *v = headers[i].value;
    size_t vl = headers[i].value_len;
    if (nl == 14) {
      // case-insensitive compare for "content-length"
      bool match = true;
      static const char *cl = "content-length";
      for (size_t j = 0; j < 14; ++j) {
        char a = n[j]; if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
        if (a != cl[j]) { match = false; break; }
      }
      if (match) {
        size_t cv = 0;
        for (size_t j = 0; j < vl; ++j) {
          char c = v[j];
          if (c >= '0' && c <= '9') cv = cv * 10 + static_cast<size_t>(c - '0');
        }
        content_length = cv;
      }
    } else if (nl == 10) {
      bool match = true;
      static const char *co = "connection";
      for (size_t j = 0; j < 10; ++j) {
        char a = n[j]; if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
        if (a != co[j]) { match = false; break; }
      }
      if (match) {
        // "close" → close
        if (vl >= 5) {
          char c0 = v[0]; if (c0 >= 'A' && c0 <= 'Z') c0 = static_cast<char>(c0 + 32);
          if (c0 == 'c') close_after = true;
        }
        if (vl >= 10) {
          char c0 = v[0]; if (c0 >= 'A' && c0 <= 'Z') c0 = static_cast<char>(c0 + 32);
          if (c0 == 'k') close_after = false;
        }
      }
    }
  }

  size_t total_needed = header_len + content_length;
  if (st.filled < total_needed) return 0;  // body not fully received

  const char *body_ptr = st.buf.data() + header_len;
  size_t      body_len = content_length;

  // Dispatch
  const char *resp = nullptr;
  size_t      resp_len = 0;

  bool is_get  = (method_len == 3 && std::memcmp(method, "GET",  3) == 0);
  bool is_post = (method_len == 4 && std::memcmp(method, "POST", 4) == 0);

  if (is_get && path_len == 6 && std::memcmp(path, "/ready", 6) == 0) {
    resp = kReadyResponse;
    resp_len = std::strlen(kReadyResponse);
  } else if (is_post && path_len == 12 &&
             std::memcmp(path, "/fraud-score", 12) == 0) {
    int count = fraud::fraud_count_payload(idx, body_ptr, body_len, nprobe);
    if (count < 0) count = 0;
    if (count > 5) count = 5;
    const std::string &b = kFraudResponses[count].buf;
    resp = b.data();
    resp_len = b.size();
  } else {
    resp = kNotFoundResponse;
    resp_len = std::strlen(kNotFoundResponse);
  }

  if (!write_all(cfd, resp, resp_len)) return -1;
  // Server is single-threaded blocking; closing after each response prevents
  // long-held keep-alive connections from blocking other clients (nginx
  // maintains many upstream conns in parallel).
  keep_alive = false;
  (void)close_after;
  return static_cast<int>(total_needed);
}

static void handle_connection(int cfd, const IvfIndex &idx, int nprobe) {
  ConnState st;

  // Slight tune: disable Nagle is a TCP thing; on AF_UNIX it's irrelevant.

  while (true) {
    if (st.filled == st.buf.size()) {
      // Request bigger than buffer → reject.
      write_all(cfd, kBadRequestResponse, std::strlen(kBadRequestResponse));
      return;
    }
    ssize_t n = ::read(cfd, st.buf.data() + st.filled,
                       st.buf.size() - st.filled);
    if (n == 0) return;                  // peer closed
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    st.filled += static_cast<size_t>(n);

    // Process as many full requests as already buffered (pipelining-friendly).
    while (true) {
      bool keep_alive = true;
      int consumed = handle_one_request(cfd, st, idx, nprobe, keep_alive);
      if (consumed < 0) return;
      if (consumed == 0) break;  // need more data
      // Shift remaining bytes to the front of the buffer.
      size_t remaining = st.filled - static_cast<size_t>(consumed);
      if (remaining > 0) {
        std::memmove(st.buf.data(),
                     st.buf.data() + consumed, remaining);
      }
      st.filled = remaining;
      if (!keep_alive) return;
    }
  }
}

// ─── warmup ──────────────────────────────────────────────────────────────────
// Pulls the bodies out of resources/example-payloads.json using a hand-rolled
// scan. The file is a JSON array of objects; each top-level object becomes
// one body. We don't need full JSON conformance — the data is well-formed and
// only contains plain objects with no nested arrays of objects at the top.
static std::vector<std::string> read_warmup_bodies(const std::string &path) {
  std::vector<std::string> out;
  std::ifstream f(path);
  if (!f.is_open()) return out;

  std::ostringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();

  // Walk braces at depth 1 (top-level array contains objects at depth 1).
  size_t i = 0, n = s.size();
  // Skip until the first '['
  while (i < n && s[i] != '[') i++;
  if (i >= n) return out;
  i++;

  while (i < n) {
    // skip ws/commas
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' ||
                     s[i] == '\n' || s[i] == ',')) i++;
    if (i >= n || s[i] == ']') break;
    if (s[i] != '{') { i++; continue; }
    size_t start = i;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    while (i < n) {
      char c = s[i];
      if (in_str) {
        if (esc) esc = false;
        else if (c == '\\') esc = true;
        else if (c == '"') in_str = false;
      } else {
        if (c == '"') in_str = true;
        else if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) { i++; break; } }
      }
      i++;
    }
    out.emplace_back(s.substr(start, i - start));
  }
  return out;
}

// ─── main ────────────────────────────────────────────────────────────────────
int main() {
  const char *env_sock = std::getenv("SOCK");
  if (!env_sock || !*env_sock) {
    std::fprintf(stderr, "SOCK env var is required\n");
    return 1;
  }
  g_sock_path = env_sock;

  const char *env_ivf = std::getenv("IVF_PATH");
  std::string ivf_path = (env_ivf && *env_ivf) ? env_ivf : "/data/ivf.bin";

  int nprobe = 1;
  if (const char *p = std::getenv("NPROBE")) {
    int v = std::atoi(p);
    if (v >= 1) nprobe = v;
  }

  int warmup_rounds = 10;
  if (const char *p = std::getenv("WARMUP")) {
    int v = std::atoi(p);
    if (v >= 0) warmup_rounds = v;
  }

  // 1. Load index
  IvfIndex idx;
  if (!load_index(ivf_path.c_str(), idx)) {
    std::fprintf(stderr, "load_index failed for %s\n", ivf_path.c_str());
    return 1;
  }
  std::fprintf(stderr,
               "FraudIndex loaded from %s (K=%u D=%u N=%u nprobe=%d)\n",
               ivf_path.c_str(), idx.K, idx.D, idx.N, nprobe);

  // 2. Pre-build responses
  build_fraud_responses();

  // 3. Warmup
  if (warmup_rounds > 0) {
    // resources/example-payloads.json — look next to the binary first, then ./resources, then /app/resources
    std::vector<std::string> candidates = {
        "resources/example-payloads.json",
        "../resources/example-payloads.json",
        "/app/resources/example-payloads.json",
    };
    if (const char *p = std::getenv("WARMUP_PAYLOADS")) {
      candidates.insert(candidates.begin(), p);
    }
    std::vector<std::string> bodies;
    std::string used_path;
    for (const auto &c : candidates) {
      bodies = read_warmup_bodies(c);
      if (!bodies.empty()) { used_path = c; break; }
    }
    if (bodies.empty()) {
      std::fprintf(stderr, "warmup: no example-payloads.json found, skipping\n");
    } else {
      auto t0 = std::chrono::steady_clock::now();
      for (int r = 0; r < warmup_rounds; ++r) {
        for (const auto &b : bodies) {
          (void)fraud::fraud_count_payload(idx, b.data(), b.size(), nprobe);
        }
      }
      auto t1 = std::chrono::steady_clock::now();
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
      std::fprintf(stderr,
                   "warmup: %dx%zu=%zu queries in %lldms (from %s)\n",
                   warmup_rounds, bodies.size(),
                   static_cast<size_t>(warmup_rounds) * bodies.size(),
                   static_cast<long long>(ms),
                   used_path.c_str());
    }
  }

  // 4. Create + bind unix socket
  ::unlink(g_sock_path.c_str());
  int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sfd < 0) {
    std::perror("socket");
    return 1;
  }

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (g_sock_path.size() >= sizeof(addr.sun_path)) {
    std::fprintf(stderr, "SOCK path too long\n");
    return 1;
  }
  std::strncpy(addr.sun_path, g_sock_path.c_str(), sizeof(addr.sun_path) - 1);

  // Mirror Puma's umask=0 → world-rw socket (nginx in another container needs it).
  mode_t old_umask = ::umask(0);
  if (::bind(sfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::umask(old_umask);
    std::perror("bind");
    return 1;
  }
  ::umask(old_umask);

  if (::listen(sfd, 4096) < 0) {
    std::perror("listen");
    return 1;
  }
  g_listen_fd.store(sfd);

  // Signal handlers for clean shutdown.
  std::signal(SIGINT,  on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  std::fprintf(stderr, "listening on unix:%s\n", g_sock_path.c_str());

  // 4.5 Fork additional worker processes. All workers share the listening fd
  // via fork(); the kernel arbitrates accept() between them. mmap of ivf.bin
  // is also shared via COW (read-only file pages).
  int workers = 1;
  if (const char *w = std::getenv("WORKERS")) {
    workers = std::max(1, std::atoi(w));
  }
  for (int i = 1; i < workers; ++i) {
    pid_t pid = ::fork();
    if (pid == 0) {
      // Child: just continues to the accept loop.
      break;
    } else if (pid < 0) {
      std::perror("fork");
    }
  }

  // 5. Accept loop (single-threaded per process)
  while (!g_shutdown.load()) {
    int cfd = ::accept(sfd, nullptr, nullptr);
    if (cfd < 0) {
      if (errno == EINTR) continue;
      if (g_shutdown.load()) break;
      std::perror("accept");
      continue;
    }
    handle_connection(cfd, idx, nprobe);
    ::close(cfd);
  }

  cleanup_socket();
  unload_index(idx);
  std::fprintf(stderr, "server exiting cleanly\n");
  return 0;
}
