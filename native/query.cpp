// Driver standalone do IVF: carrega ivf.bin, calcula score pra uma query
// hardcoded, imprime os top-5 e o veredito.

#include "ivf_index.hpp"
#include <iostream>

using namespace ivf;
using namespace std;

// payload tx-1329056812 (last_transaction = null)
alignas(64) float QUERY[14] = {
    0.004112f,  0.166667f,  0.050000f, 0.782609f, 0.333333f,
    -1.000000f, -1.000000f, 0.029233f, 0.150000f, 0.000000f,
    1.000000f,  0.000000f,  0.150000f, 0.006025f,
};

int main(void) {
  IvfIndex idx;
  if (!load_index("ivf.bin", idx)) return 1;

  cout << "K=" << idx.K << "  D=" << idx.D << "  N=" << idx.N << endl;

  uint32_t c = nearest_centroid(idx, QUERY);
  cout << "best_centroid: " << c << endl;

  uint32_t top[TOP_K];
  top5_in_cluster(idx, QUERY, c, top);

  cout << "top-5:\n";
  int frauds = 0;
  for (int k = 0; k < TOP_K; ++k) {
    uint8_t lbl = idx.labels[top[k]];
    frauds += lbl;
    cout << "  idx=" << top[k]
         << "  label=" << (lbl ? "fraud" : "legit") << "\n";
  }

  float fraud_score = frauds / static_cast<float>(TOP_K);
  bool  approved    = fraud_score < 0.6f;

  cout << "fraud_score: " << fraud_score
       << "  approved: " << (approved ? "true" : "false") << endl;

  unload_index(idx);
  return 0;
}
