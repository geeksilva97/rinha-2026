#include <cerrno>
#include <cstring>
#include <experimental/simd>
#include <fcntl.h> // open, O_RDONLY
#include <iostream>
#include <limits>
#include <sys/mman.h> // mmap, munmap, PROT_READ, MAP_PRIVATE
#include <sys/stat.h> // fstat, struct stat
#include <unistd.h>   // close

using namespace std;
namespace stdx = std::experimental;

constexpr uint32_t DIM = 14;

// Distância L2² entre dois vetores DIM-dim.
// Em ARM NEON (M4): W=4 → 3 SIMD ops cobrem 12 floats, tail de 2 escalar.
// Em x86 AVX2 (Haswell): W=8 → 1 SIMD op cobre 8 floats, tail de 6 escalar.
// Em x86 AVX-512: W=16 → tudo escalar (DIM<W), o for SIMD nem entra.
static inline float dist_sq(const float *a, const float *b) {
  using simd_f         = stdx::native_simd<float>;
  constexpr size_t W   = simd_f::size();

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

// payload tx-1329056812 (last_transaction = null)
float query[14] = {
    0.004112f,  0.166667f,  0.050000f, 0.782609f, 0.333333f,
    -1.000000f, -1.000000f, 0.029233f, 0.150000f, 0.000000f,
    1.000000f,  0.000000f,  0.150000f, 0.006025f,
};

const auto PATH = "ivf.bin";

int main(void) {
  int fd = open(PATH, O_RDONLY);
  if (fd == -1) {
    cerr << "open: " << strerror(errno) << endl;
    return 1;
  }

  struct stat st;
  fstat(fd, &st);
  void *base = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

  const char *p = static_cast<const char *>(base);
  const uint32_t *header = reinterpret_cast<const uint32_t *>(p);
  uint32_t K = header[0];
  uint32_t D = header[1];
  uint32_t N = header[2];
  const float *centroids = reinterpret_cast<const float *>(p + 12);
  const uint32_t *offsets =
      reinterpret_cast<const uint32_t *>(p + 12 + K * D * 4);
  const float *vectors =
      reinterpret_cast<const float *>(p + 12 + K * D * 4 + (K + 1) * 4);
  const uint8_t *labels = reinterpret_cast<const uint8_t *>(
      p + 12 + K * D * 4 + (K + 1) * 4 + N * D * 4);

  float best_dist = std::numeric_limits<float>::max();
  int best_centroid = 0;

  for (uint32_t i = 0; i < K; ++i) {
    float dist = dist_sq(query, centroids + i * DIM);
    if (dist < best_dist) {
      best_dist = dist;
      best_centroid = i;
    }
  }

  cout << "best_centroid: " << best_centroid << "  dist²: " << best_dist << endl;

  // ─── TOP-5 dentro do cluster best_centroid ──────────────────────────
  uint32_t begin = offsets[best_centroid];
  uint32_t end   = offsets[best_centroid + 1];

  // Array de 5 pares (dist², índice global do vetor) mantido sempre ordenado
  // crescente. top[0] é o mais próximo; top[4] é o "pior dos 5".
  // Inicializa com infinito pra que qualquer dist² real seja inserida.
  constexpr int TOP_K = 5;
  float    top_dist[TOP_K] = { std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max() };
  uint32_t top_idx[TOP_K]  = { 0, 0, 0, 0, 0 };

  for (uint32_t i = begin; i < end; ++i) {
    float dist = dist_sq(query, vectors + i * DIM);

    // Se não cabe nem no pior do top-5, descarta
    if (dist >= top_dist[TOP_K - 1]) continue;

    // Acha onde inserir (insertion sort em array de 5)
    int pos = TOP_K - 1;
    while (pos > 0 && top_dist[pos - 1] > dist) {
      top_dist[pos] = top_dist[pos - 1];
      top_idx[pos]  = top_idx[pos - 1];
      --pos;
    }
    top_dist[pos] = dist;
    top_idx[pos]  = i;
  }

  // ─── fraud_score ────────────────────────────────────────────────────
  int frauds = 0;
  cout << "top-5:\n";
  for (int k = 0; k < TOP_K; ++k) {
    uint8_t lbl = labels[top_idx[k]];
    frauds += lbl;
    cout << "  idx=" << top_idx[k]
         << "  dist²=" << top_dist[k]
         << "  label=" << (lbl ? "fraud" : "legit") << "\n";
  }

  float fraud_score = frauds / 5.0f;
  bool  approved    = fraud_score < 0.6f;

  cout << "fraud_score: " << fraud_score
       << "  approved: " << (approved ? "true" : "false") << endl;

  return 0;
}
