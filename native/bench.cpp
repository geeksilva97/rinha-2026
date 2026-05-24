// Bench: SIMD vs escalar na distância L2² (mesma query, mesmo IVF).
//
// Mede só o caminho quente: centroid search + top-5 dentro do cluster.
// Roda N iterações por versão pra amortizar ruído.

#include <cerrno>
#include <chrono>
#include <cstring>
#include <experimental/simd>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;
namespace stdx = std::experimental;

constexpr uint32_t DIM = 14;
constexpr int      TOP_K = 5;
constexpr int      ITERS = 100'000;

// payload tx-1329056812
alignas(64) float query[14] = {
    0.004112f,  0.166667f,  0.050000f, 0.782609f, 0.333333f,
    -1.000000f, -1.000000f, 0.029233f, 0.150000f, 0.000000f,
    1.000000f,  0.000000f,  0.150000f, 0.006025f,
};

// ─── escalar ─────────────────────────────────────────────────────────
static inline float dist_sq_scalar(const float *a, const float *b) {
  float d = 0;
  for (uint32_t i = 0; i < DIM; ++i) {
    float diff = a[i] - b[i];
    d += diff * diff;
  }
  return d;
}

// ─── SIMD ────────────────────────────────────────────────────────────
static inline float dist_sq_simd(const float *a, const float *b) {
  using simd_f = stdx::native_simd<float>;
  constexpr size_t W = simd_f::size();

  simd_f acc{0.0f};
  size_t i = 0;
  for (; i + W <= DIM; i += W) {
    simd_f va(a + i, stdx::element_aligned);
    simd_f vb(b + i, stdx::element_aligned);
    simd_f diff = va - vb;
    acc += diff * diff;
  }
  float result = stdx::reduce(acc);
  for (; i < DIM; ++i) {
    float diff = a[i] - b[i];
    result += diff * diff;
  }
  return result;
}

// ─── busca completa: centroid + top-5 ────────────────────────────────
template <auto DIST>
__attribute__((noinline))
float run_query(const float *centroids, const uint32_t *offsets,
                const float *vectors, const uint8_t *labels, uint32_t K) {
  float best_dist = std::numeric_limits<float>::max();
  uint32_t best_c = 0;
  for (uint32_t i = 0; i < K; ++i) {
    float d = DIST(query, centroids + i * DIM);
    if (d < best_dist) { best_dist = d; best_c = i; }
  }

  uint32_t begin = offsets[best_c];
  uint32_t end   = offsets[best_c + 1];

  float    top_d[TOP_K] = { std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max() };
  uint32_t top_i[TOP_K] = { 0, 0, 0, 0, 0 };

  for (uint32_t i = begin; i < end; ++i) {
    float d = DIST(query, vectors + i * DIM);
    if (d >= top_d[TOP_K - 1]) continue;
    int p = TOP_K - 1;
    while (p > 0 && top_d[p - 1] > d) {
      top_d[p] = top_d[p - 1];
      top_i[p] = top_i[p - 1];
      --p;
    }
    top_d[p] = d;
    top_i[p] = i;
  }

  int frauds = 0;
  for (int k = 0; k < TOP_K; ++k) frauds += labels[top_i[k]];
  return frauds / 5.0f;
}

int main() {
  int fd = open("ivf.bin", O_RDONLY);
  if (fd == -1) { cerr << "open: " << strerror(errno) << endl; return 1; }
  struct stat st; fstat(fd, &st);
  void *base = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

  const char     *p       = static_cast<const char *>(base);
  const uint32_t *hdr     = reinterpret_cast<const uint32_t *>(p);
  uint32_t        K       = hdr[0];
  uint32_t        N       = hdr[2];
  const float    *cent    = reinterpret_cast<const float *>(p + 12);
  const uint32_t *offs    = reinterpret_cast<const uint32_t *>(p + 12 + K * DIM * 4);
  const float    *vecs    = reinterpret_cast<const float *>(p + 12 + K * DIM * 4 + (K + 1) * 4);
  const uint8_t  *labs    = reinterpret_cast<const uint8_t *>(p + 12 + K * DIM * 4 + (K + 1) * 4 + N * DIM * 4);

  cout << "K=" << K << "  N=" << N << "  DIM=" << DIM
       << "  SIMD width=" << stdx::native_simd<float>::size() << endl;
  cout << "iterações por versão: " << ITERS << endl << endl;

  // Aquece o page cache (mmap lazy) com 100 chamadas
  for (int i = 0; i < 100; ++i) run_query<dist_sq_simd>(cent, offs, vecs, labs, K);

  using clock = std::chrono::steady_clock;
  using us    = std::chrono::microseconds;

  // ─── ESCALAR ──────────────────────────────────────────────────────
  volatile float sink_s = 0;
  auto t0 = clock::now();
  for (int i = 0; i < ITERS; ++i)
    sink_s += run_query<dist_sq_scalar>(cent, offs, vecs, labs, K);
  auto t1 = clock::now();
  auto dt_scalar = std::chrono::duration_cast<us>(t1 - t0).count();

  // ─── SIMD ─────────────────────────────────────────────────────────
  volatile float sink_v = 0;
  auto t2 = clock::now();
  for (int i = 0; i < ITERS; ++i)
    sink_v += run_query<dist_sq_simd>(cent, offs, vecs, labs, K);
  auto t3 = clock::now();
  auto dt_simd = std::chrono::duration_cast<us>(t3 - t2).count();

  cout << "escalar: " << dt_scalar << " µs total  ("
       << (double)dt_scalar / ITERS << " µs/query)" << endl;
  cout << "SIMD:    " << dt_simd   << " µs total  ("
       << (double)dt_simd   / ITERS << " µs/query)" << endl;
  cout << "speedup: " << (double)dt_scalar / dt_simd << "x" << endl;

  // garante que o compilador não otimize fora
  (void)sink_s; (void)sink_v;
  return 0;
}
