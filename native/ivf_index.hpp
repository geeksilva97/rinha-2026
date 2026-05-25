// IVF index — int16 quantized layout.
//
// Vectors and centroids are stored as int16 (one global scale; queries
// quantized at request time using the same scale). dist² is computed in
// integer space; top-K ordering is preserved because the transform is a
// uniform linear scaling.
//
// Storage layout per ivf.bin:
//   HEADER (16 bytes):
//     uint32 K
//     uint32 D    (always 14)
//     uint32 N
//     float  scale (q = round(v * scale), e.g. 32767 for normalized data)
//   CENTROIDS:  K  × STRIDE × int16    (STRIDE=16 padded for AVX2)
//   OFFSETS:    (K+1) × uint32
//   VECTORS:    N  × STRIDE × int16
//   LABELS:     N  × uint8
//
// The padded slots (DIM..STRIDE-1) are zero and contribute 0 to dist².

#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__AVX2__)
#  include <immintrin.h>
#  define IVF_HAS_AVX2 1
#else
#  define IVF_HAS_AVX2 0
#endif

namespace ivf {

constexpr uint32_t DIM    = 14;
constexpr uint32_t STRIDE = 16;   // 14 dims padded to 16 int16 = 32 bytes, one AVX2 reg
constexpr int      TOP_K  = 5;

struct IvfIndex {
  void *base   = nullptr;
  size_t size  = 0;
  int    fd    = -1;

  uint32_t K = 0;
  uint32_t D = 0;
  uint32_t N = 0;
  float    scale = 1.0f;   // q = round(v * scale)

  const int16_t  *centroids = nullptr;  // K × STRIDE
  const uint32_t *offsets   = nullptr;  // K + 1
  const int16_t  *vectors   = nullptr;  // N × STRIDE
  const uint8_t  *labels    = nullptr;
};

// Float-domain dist² — used at training time (k_means.cpp) on raw float
// vectors before quantization. The production query path uses the int16
// version below.
static inline float dist_sq(const float *a, const float *b) {
  float d = 0;
  for (uint32_t i = 0; i < DIM; ++i) {
    float diff = a[i] - b[i];
    d += diff * diff;
  }
  return d;
}

// Squared L2 distance in quantized int16 space. Returns int32 sum of squared
// differences over STRIDE lanes (DIM real + STRIDE-DIM zeros, which add 0).
// Max value: 14 dims × (2×32767)² = ~6.0e10 → fits int32 (2.1e9)? NO.
// 14 × (65534)² = 6.01e10 — that EXCEEDS int32 range (2.1e9). We need int64.
// In practice for normalized vectors the differences are small (each ~scale × 2),
// but worst-case theoretical requires int64. Use int64_t for safety.
static inline int64_t dist_sq(const int16_t *a, const int16_t *b) {
#if IVF_HAS_AVX2
  // Load 16 int16 from each (one full AVX2 256-bit register).
  __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a));
  __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b));
  __m256i diff = _mm256_sub_epi16(va, vb);      // 16 int16 diffs (safe: scale=16383 → max diff = 32766 < INT16_MAX)
  __m256i sq   = _mm256_madd_epi16(diff, diff); // 8 int32 (each = d²+d² ≤ 2 × 32766² ≈ 2.147e9 ≤ INT32_MAX)

  // Sum of 8 int32 can reach 8 × 2.147e9 = 1.7e10, OVERFLOWS int32.
  // Extend each int32 to int64 before summing.
  __m256i sq_lo64 = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(sq));        // 4 int64 (sq[0..3])
  __m256i sq_hi64 = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(sq, 1));   // 4 int64 (sq[4..7])
  __m256i sum64   = _mm256_add_epi64(sq_lo64, sq_hi64);                       // 4 int64

  // Horizontal sum 4 int64 → scalar.
  __m128i lo = _mm256_castsi256_si128(sum64);
  __m128i hi = _mm256_extracti128_si256(sum64, 1);
  __m128i s2 = _mm_add_epi64(lo, hi);                                         // 2 int64
  return _mm_extract_epi64(s2, 0) + _mm_extract_epi64(s2, 1);
#else
  int64_t d = 0;
  for (uint32_t i = 0; i < DIM; ++i) {
    int32_t x = static_cast<int32_t>(a[i]) - static_cast<int32_t>(b[i]);
    d += static_cast<int64_t>(x) * static_cast<int64_t>(x);
  }
  return d;
#endif
}

// Quantize a float query into a padded int16 buffer (STRIDE slots).
// out[DIM..STRIDE-1] are zeroed by the caller (or here).
static inline void quantize_query(const float *q, float scale, int16_t *out) {
  // Caps at ±16383 (= INT16_MAX/2). Together with stored vectors quantized
  // at the same cap, max diff is 32766 → fits int16, avoids vpsubw wrap.
  for (uint32_t d = 0; d < DIM; ++d) {
    float v = std::round(q[d] * scale);
    if (v >  16383.0f) v =  16383.0f;
    if (v < -16383.0f) v = -16383.0f;
    out[d] = static_cast<int16_t>(v);
  }
  for (uint32_t d = DIM; d < STRIDE; ++d) out[d] = 0;
}

inline bool load_index(const char *path, IvfIndex &idx) {
  idx.fd = open(path, O_RDONLY);
  if (idx.fd == -1) {
    std::cerr << "open: " << std::strerror(errno) << std::endl;
    return false;
  }

  struct stat st;
  if (fstat(idx.fd, &st) == -1) { close(idx.fd); return false; }
  idx.size = st.st_size;

  idx.base = mmap(nullptr, idx.size, PROT_READ, MAP_PRIVATE, idx.fd, 0);
  if (idx.base == MAP_FAILED) { close(idx.fd); return false; }
  madvise(idx.base, idx.size, MADV_RANDOM);

  const char *p = static_cast<const char *>(idx.base);
  const uint32_t *u = reinterpret_cast<const uint32_t *>(p);
  idx.K = u[0];
  idx.D = u[1];
  idx.N = u[2];
  idx.scale = *reinterpret_cast<const float *>(p + 12);

  if (idx.D != DIM) {
    std::cerr << "header DIM=" << idx.D << ", expected " << DIM << std::endl;
    munmap(idx.base, idx.size);
    close(idx.fd);
    return false;
  }

  std::cerr << "ivf.mmap path=" << path
            << " dev=" << st.st_dev
            << " inode=" << st.st_ino
            << " size=" << st.st_size
            << " K=" << idx.K << " N=" << idx.N
            << " scale=" << idx.scale
            << " (int16, STRIDE=" << STRIDE << ")"
            << " addr=" << idx.base
            << "-" << static_cast<const void *>(
                       static_cast<const char *>(idx.base) + idx.size)
            << std::endl;

  // Section offsets (in bytes from base):
  //   centroids: 20 (= 12 hdr + 4 scale, aligned for int16 — fine)
  //   offsets:   20 + K * STRIDE * 2
  //   vectors:   offsets + (K+1) * 4
  //   labels:    vectors + N * STRIDE * 2
  const size_t cent_bytes = static_cast<size_t>(idx.K) * STRIDE * sizeof(int16_t);
  const size_t off_bytes  = (static_cast<size_t>(idx.K) + 1) * sizeof(uint32_t);
  const size_t vec_bytes  = static_cast<size_t>(idx.N) * STRIDE * sizeof(int16_t);

  idx.centroids = reinterpret_cast<const int16_t *>(p + 16);
  idx.offsets   = reinterpret_cast<const uint32_t *>(p + 16 + cent_bytes);
  idx.vectors   = reinterpret_cast<const int16_t *>(p + 16 + cent_bytes + off_bytes);
  idx.labels    = reinterpret_cast<const uint8_t *>(p + 16 + cent_bytes + off_bytes + vec_bytes);

  return true;
}

inline void unload_index(IvfIndex &idx) {
  if (idx.base && idx.base != MAP_FAILED) munmap(idx.base, idx.size);
  if (idx.fd >= 0) close(idx.fd);
  idx = {};
}

// Finds the nearest centroid to a quantized query (brute force across K).
inline uint32_t nearest_centroid(const IvfIndex &idx, const int16_t *query) {
  int64_t  best = std::numeric_limits<int64_t>::max();
  uint32_t best_c = 0;
  for (uint32_t i = 0; i < idx.K; ++i) {
    int64_t d = dist_sq(query, idx.centroids + i * STRIDE);
    if (d < best) { best = d; best_c = i; }
  }
  return best_c;
}

// Top-N nearest centroids. out_clusters[] must have capacity nprobe.
inline void top_n_centroids(const IvfIndex &idx, const int16_t *query,
                            int nprobe, uint32_t *out_clusters) {
  int64_t *top_d = static_cast<int64_t *>(alloca(nprobe * sizeof(int64_t)));
  for (int k = 0; k < nprobe; ++k) {
    top_d[k]        = std::numeric_limits<int64_t>::max();
    out_clusters[k] = 0;
  }

  for (uint32_t i = 0; i < idx.K; ++i) {
    int64_t d = dist_sq(query, idx.centroids + i * STRIDE);
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

inline void merge_top5_from_cluster(const IvfIndex &idx, const int16_t *query,
                                    uint32_t cluster,
                                    int64_t top_d[TOP_K],
                                    uint32_t out_idx[TOP_K]) {
  uint32_t begin = idx.offsets[cluster];
  uint32_t end   = idx.offsets[cluster + 1];

  for (uint32_t i = begin; i < end; ++i) {
    int64_t d = dist_sq(query, idx.vectors + i * STRIDE);
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

// End-to-end score: nprobe nearest centroids → global top-5 → fraud count / 5.
// `query` is the int16-quantized query (STRIDE lanes).
inline float ivf_score(const IvfIndex &idx, const int16_t *query, int nprobe = 1) {
  int64_t  top_d[TOP_K];
  uint32_t top_i[TOP_K];
  for (int k = 0; k < TOP_K; ++k) {
    top_d[k] = std::numeric_limits<int64_t>::max();
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
