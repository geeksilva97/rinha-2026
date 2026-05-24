// IVF index: carrega o ivf.bin via mmap e responde queries kNN (k=5).
//
// Uso típico:
//     IvfIndex idx;
//     if (!load_index("ivf.bin", idx)) { ... }
//     float score = ivf_score(idx, query_14_floats);
//     unload_index(idx);
//
// O score retornado é num_frauds / 5.0f. O caller decide o threshold.

#pragma once

#include <cerrno>
#include <cstring>
#include <experimental/simd>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ivf {

namespace stdx = std::experimental;

constexpr uint32_t DIM   = 14;
constexpr int      TOP_K = 5;

struct IvfIndex {
  // mmap bookkeeping (pra unload depois)
  void *base   = nullptr;
  size_t size  = 0;
  int    fd    = -1;

  // header (lido do arquivo)
  uint32_t K = 0;
  uint32_t D = 0;
  uint32_t N = 0;

  // ponteiros pras seções dentro do mmap
  const float    *centroids = nullptr;
  const uint32_t *offsets   = nullptr;
  const float    *vectors   = nullptr;
  const uint8_t  *labels    = nullptr;
};

// Distância L2² entre dois vetores DIM-dim, via std::experimental::simd.
// NEON (W=4): 3 SIMD ops + 2 escalar.  AVX2 (W=8): 1 SIMD + 6 escalar.
static inline float dist_sq(const float *a, const float *b) {
  using simd_f       = stdx::native_simd<float>;
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

// Abre + mmap + valida o header. Retorna true em sucesso.
// Em falha, idx fica em estado inválido (não chamar unload).
inline bool load_index(const char *path, IvfIndex &idx) {
  idx.fd = open(path, O_RDONLY);
  if (idx.fd == -1) {
    std::cerr << "open: " << std::strerror(errno) << std::endl;
    return false;
  }

  struct stat st;
  if (fstat(idx.fd, &st) == -1) {
    close(idx.fd);
    return false;
  }
  idx.size = st.st_size;

  idx.base = mmap(nullptr, idx.size, PROT_READ, MAP_PRIVATE, idx.fd, 0);
  if (idx.base == MAP_FAILED) {
    close(idx.fd);
    return false;
  }

  // Hint pro kernel: padrão de acesso é random (não sequencial)
  madvise(idx.base, idx.size, MADV_RANDOM);

  const char     *p   = static_cast<const char *>(idx.base);
  const uint32_t *hdr = reinterpret_cast<const uint32_t *>(p);
  idx.K = hdr[0];
  idx.D = hdr[1];
  idx.N = hdr[2];

  if (idx.D != DIM) {
    std::cerr << "header DIM=" << idx.D << ", esperado " << DIM << std::endl;
    munmap(idx.base, idx.size);
    close(idx.fd);
    return false;
  }

  idx.centroids = reinterpret_cast<const float *>(p + 12);
  idx.offsets   = reinterpret_cast<const uint32_t *>(p + 12 + idx.K * DIM * 4);
  idx.vectors   = reinterpret_cast<const float *>(
      p + 12 + idx.K * DIM * 4 + (idx.K + 1) * 4);
  idx.labels = reinterpret_cast<const uint8_t *>(
      p + 12 + idx.K * DIM * 4 + (idx.K + 1) * 4 + idx.N * DIM * 4);

  return true;
}

inline void unload_index(IvfIndex &idx) {
  if (idx.base && idx.base != MAP_FAILED) munmap(idx.base, idx.size);
  if (idx.fd >= 0) close(idx.fd);
  idx = {};
}

// Acha o centróide mais próximo do query (brute force entre os K centróides).
inline uint32_t nearest_centroid(const IvfIndex &idx, const float *query) {
  float    best = std::numeric_limits<float>::max();
  uint32_t best_c = 0;
  for (uint32_t i = 0; i < idx.K; ++i) {
    float d = dist_sq(query, idx.centroids + i * DIM);
    if (d < best) { best = d; best_c = i; }
  }
  return best_c;
}

// Top-5 vizinhos dentro de um cluster. Resultado vai em out_idx (índices
// globais dos vetores). Insertion sort em buffer de 5.
inline void top5_in_cluster(const IvfIndex &idx, const float *query,
                            uint32_t cluster, uint32_t out_idx[TOP_K]) {
  uint32_t begin = idx.offsets[cluster];
  uint32_t end   = idx.offsets[cluster + 1];

  float top_d[TOP_K];
  for (int k = 0; k < TOP_K; ++k) {
    top_d[k] = std::numeric_limits<float>::max();
    out_idx[k] = 0;
  }

  for (uint32_t i = begin; i < end; ++i) {
    float d = dist_sq(query, idx.vectors + i * DIM);
    if (d >= top_d[TOP_K - 1]) continue;

    int p = TOP_K - 1;
    while (p > 0 && top_d[p - 1] > d) {
      top_d[p]   = top_d[p - 1];
      out_idx[p] = out_idx[p - 1];
      --p;
    }
    top_d[p]   = d;
    out_idx[p] = i;
  }
}

// Score completo: encontra cluster mais próximo, top-5 lá dentro, conta
// fraudes / 5.
inline float ivf_score(const IvfIndex &idx, const float *query) {
  uint32_t c = nearest_centroid(idx, query);

  uint32_t top[TOP_K];
  top5_in_cluster(idx, query, c, top);

  int frauds = 0;
  for (int k = 0; k < TOP_K; ++k) frauds += idx.labels[top[k]];
  return frauds / static_cast<float>(TOP_K);
}

} // namespace ivf
