// IVF index: loads ivf.bin via mmap and answers kNN queries (k=5).
//
// Typical use:
//     IvfIndex idx;
//     if (!load_index("ivf.bin", idx)) { ... }
//     float score = ivf_score(idx, query_14_floats);
//     unload_index(idx);
//
// The returned score is num_frauds / 5.0f. Caller decides the threshold.

#pragma once

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// std::experimental::simd is only implemented in libstdc++ (GCC).
// Clang/libc++ ships the header but it's missing bits (e.g. reduce). Restrict to GCC.
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
  // mmap bookkeeping (for later unload)
  void *base   = nullptr;
  size_t size  = 0;
  int    fd    = -1;

  // header (lido do arquivo)
  uint32_t K = 0;
  uint32_t D = 0;
  uint32_t N = 0;

  // pointers into each section of the mmap region
  const float    *centroids = nullptr;
  const uint32_t *offsets   = nullptr;
  const float    *vectors   = nullptr;
  const uint8_t  *labels    = nullptr;
};

// Squared L2 distance between two DIM-dim vectors.
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

// Opens, mmaps, and validates the header. Returns true on success.
// On failure, idx is left in an invalid state (do not call unload).
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

  // Hint to the kernel: access pattern is random (not sequential)
  madvise(idx.base, idx.size, MADV_RANDOM);

  // One-shot boot log: dev/inode/size + mapped range. Two containers running
  // the same image show identical inode + identical mapped address — proof
  // that the kernel can share page-cache pages across both processes.
  std::cerr << "ivf.mmap path=" << path
            << " dev=" << st.st_dev
            << " inode=" << st.st_ino
            << " size=" << st.st_size
            << " addr=" << idx.base
            << "-" << static_cast<const void *>(
                       static_cast<const char *>(idx.base) + idx.size)
            << std::endl;

  const char     *p   = static_cast<const char *>(idx.base);
  const uint32_t *hdr = reinterpret_cast<const uint32_t *>(p);
  idx.K = hdr[0];
  idx.D = hdr[1];
  idx.N = hdr[2];

  if (idx.D != DIM) {
    std::cerr << "header DIM=" << idx.D << ", expected " << DIM << std::endl;
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

// Finds the nearest centroid to the query (brute force across the K centroids).
inline uint32_t nearest_centroid(const IvfIndex &idx, const float *query) {
  float    best = std::numeric_limits<float>::max();
  uint32_t best_c = 0;
  for (uint32_t i = 0; i < idx.K; ++i) {
    float d = dist_sq(query, idx.centroids + i * DIM);
    if (d < best) { best = d; best_c = i; }
  }
  return best_c;
}

// Top-N nearest centroids. out_clusters[] must have capacity N.
// Result is sorted ascending by distance. Insertion sort over N.
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

// Updates the global top-K in a single pass over one cluster. Caller keeps
// the state (top_d, out_idx) across calls to scan multiple clusters.
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

// End-to-end score: finds the nprobe nearest centroids, global top-5 across
// all of their vectors, returns fraud count / 5.
inline float ivf_score(const IvfIndex &idx, const float *query, int nprobe = 1) {
  // Global top-5 state, persistent across clusters
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
