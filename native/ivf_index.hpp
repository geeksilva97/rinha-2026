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
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// std::experimental::simd só está implementado em libstdc++ (GCC).
// Clang/libc++ tem o header mas faltam pedaços (ex: reduce). Restringe a GCC.
#if defined(__GNUC__) && !defined(__clang__) && __has_include(<experimental/simd>)
#  include <experimental/simd>
#  define IVF_HAS_SIMD 1
#else
#  define IVF_HAS_SIMD 0
#endif

namespace ivf {

#if IVF_HAS_SIMD
namespace stdx = std::experimental;
#endif

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

// Distância L2² entre dois vetores DIM-dim.
static inline float dist_sq(const float *a, const float *b) {
#if IVF_HAS_SIMD
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
#else
  // Fallback escalar (compilador auto-vetoriza com -O2 + DIM constexpr)
  float d = 0;
  for (uint32_t i = 0; i < DIM; ++i) {
    float diff = a[i] - b[i];
    d += diff * diff;
  }
  return d;
#endif
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

// Top-N centróides mais próximos. out_clusters[] precisa ter capacidade N.
// Resultado ordenado crescente por distância. Insertion sort sobre N.
inline void top_n_centroids(const IvfIndex &idx, const float *query,
                            int nprobe, uint32_t *out_clusters) {
  float *top_d = static_cast<float *>(alloca(nprobe * sizeof(float)));
  for (int k = 0; k < nprobe; ++k) {
    top_d[k]        = std::numeric_limits<float>::max();
    out_clusters[k] = 0;
  }

  for (uint32_t i = 0; i < idx.K; ++i) {
    float d = dist_sq(query, idx.centroids + i * DIM);
    if (d >= top_d[nprobe - 1]) continue;

    int p = nprobe - 1;
    while (p > 0 && top_d[p - 1] > d) {
      top_d[p]        = top_d[p - 1];
      out_clusters[p] = out_clusters[p - 1];
      --p;
    }
    top_d[p]        = d;
    out_clusters[p] = i;
  }
}

// Top-K global atualizado em uma passada por um cluster. Mantém o estado
// (top_d, out_idx) entre chamadas pra varrer múltiplos clusters.
inline void merge_top5_from_cluster(const IvfIndex &idx, const float *query,
                                    uint32_t cluster,
                                    float top_d[TOP_K],
                                    uint32_t out_idx[TOP_K]) {
  uint32_t begin = idx.offsets[cluster];
  uint32_t end   = idx.offsets[cluster + 1];

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

// Score completo: encontra os nprobe centróides mais próximos, top-5 global
// entre todos os vetores deles, conta fraudes / 5.
inline float ivf_score(const IvfIndex &idx, const float *query, int nprobe = 1) {
  // Estado do top-5 global, persistente entre clusters
  float    top_d[TOP_K];
  uint32_t top_i[TOP_K];
  for (int k = 0; k < TOP_K; ++k) {
    top_d[k] = std::numeric_limits<float>::max();
    top_i[k] = 0;
  }

  if (nprobe <= 1) {
    uint32_t c = nearest_centroid(idx, query);
    merge_top5_from_cluster(idx, query, c, top_d, top_i);
  } else {
    uint32_t *clusters = static_cast<uint32_t *>(alloca(nprobe * sizeof(uint32_t)));
    top_n_centroids(idx, query, nprobe, clusters);
    for (int n = 0; n < nprobe; ++n) {
      merge_top5_from_cluster(idx, query, clusters[n], top_d, top_i);
    }
  }

  int frauds = 0;
  for (int k = 0; k < TOP_K; ++k) frauds += idx.labels[top_i[k]];
  return frauds / static_cast<float>(TOP_K);
}

} // namespace ivf
